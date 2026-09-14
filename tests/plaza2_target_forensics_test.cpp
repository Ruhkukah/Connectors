#include "plaza2_target_forensics.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace cg = moex::plaza2::cgate;
namespace gen = moex::plaza2::generated;
using Kind = cg::Plaza2ListenerEventKind;
constexpr auto stream = gen::StreamCode::kFortsRefdataRepl;
constexpr auto table = gen::TableCode::kFortsRefdataReplFutSessContents;
constexpr auto max_revision = std::numeric_limits<std::int64_t>::max();

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
Plaza2TargetForensics make_probe() {
    Plaza2TargetForensics p;
    p.isin = 1;
    p.session = 2;
    p.expected_symbol = "TEST";
    return p;
}
void event(Plaza2TargetForensics& p, Kind kind, std::int64_t boundary = 0, gen::TableCode target_table = table) {
    p.observe({.kind = kind,
               .stream_code = stream,
               .table_code = target_table,
               .unsigned_value = 7,
               .signed_value = boundary});
}
void capture(Plaza2TargetForensics& p, std::int64_t rev) {
    using F = gen::FieldCode;
    const std::array<cg::Plaza2DecodedFieldValue, 3> fields{{
        {.field_code = F::kFortsRefdataReplFutSessContentsIsinId, .signed_value = 1},
        {.field_code = F::kFortsRefdataReplFutSessContentsSessId, .signed_value = 2},
        {.field_code = F::kFortsRefdataReplFutSessContentsReplRev, .signed_value = rev},
    }};
    require(p.wants({.kind = Kind::StreamData, .stream_code = stream, .table_code = table, .fields = fields}),
            "target revision must be capturable, including reused revision after MAX");
    cg::Plaza2ForensicRow row;
    row.fields = {{.name = "replRev", .independent_value = std::to_string(rev), .equal = true},
                  {.name = "isin_id", .independent_value = "1", .equal = true},
                  {.name = "sess_id", .independent_value = "2", .equal = true},
                  {.name = "isin", .independent_value = "TEST", .equal = true}};
    p.capture(std::move(row));
}
void commit(Plaza2TargetForensics& p, std::int64_t rev) {
    event(p, Kind::TransactionBegin);
    capture(p, rev);
    event(p, Kind::TransactionCommit);
}
int main() {
    try {
        // Both undrained raw evidence and an already-drained proof obey the boundary.
        for (const bool drained : {false, true}) {
            for (const auto boundary : {std::int64_t{90}, std::int64_t{100}, std::int64_t{101}, max_revision}) {
                auto p = make_probe();
                commit(p, 100);
                if (drained)
                    (void)p.drain(true);
                event(p, Kind::ClearDeleted, boundary);
                const auto output = p.drain(true);
                const bool survives = boundary <= 100;
                require(p.identity_verified() == survives, "ordinary strict boundary/MAX proof retention");
                require(drained || output.empty() == !survives, "stale undrained evidence cannot reauthorize");
                require(!p.failed, "legal retention is not protocol failure");
            }
        }
        {
            auto p = make_probe();
            commit(p, 100);
            event(p, Kind::ClearDeleted, max_revision, gen::TableCode::kFortsRefdataReplOptSessContents);
            require(!p.drain(true).empty() && p.identity_verified(), "unrelated table cannot retire target");
        }
        for (const auto old_revision : {std::int64_t{100}, std::int64_t{1}}) {
            auto p = make_probe();
            commit(p, old_revision);
            event(p, Kind::TransactionBegin);
            event(p, Kind::ClearDeleted, max_revision);
            require(p.has_committed() && !p.identity_verified(), "clear staged and terms blocked during TN");
            capture(p, 1);
            event(p, Kind::TransactionCommit);
            const auto output = p.drain(true);
            require(!p.failed && p.identity_verified() && output.find("\"repl_rev\":1,") != std::string::npos &&
                        output.find("\"repl_rev\":100,") == std::string::npos,
                    "MAX retires old publication and retains fresh low revision");
        }
        {
            auto p = make_probe();
            commit(p, 100);
            event(p, Kind::TransactionBegin);
            event(p, Kind::ClearDeleted, 101);
            require(p.has_committed() && !p.identity_verified(), "ordinary deletion staged until commit");
            (void)p.drain(true);
            require(!p.identity_verified(), "drain cannot authorize an open transaction");
            event(p, Kind::TransactionCommit);
            require(!p.identity_verified(), "commit retires even an already-drained stale proof");
        }
        {
            auto p = make_probe();
            event(p, Kind::TransactionBegin);
            capture(p, 100);
            event(p, Kind::ClearDeleted, max_revision);
            capture(p, 1);
            event(p, Kind::TransactionCommit);
            const auto output = p.drain(true);
            require(p.identity_verified() && output.find("\"repl_rev\":100,") == std::string::npos,
                    "MAX also retires pre-marker staged rows");
        }
        for (const auto control : {Kind::LifeNum, Kind::Close, Kind::Open}) {
            auto p = make_probe();
            commit(p, 100);
            event(p, control);
            require(p.drain(true).empty() && !p.identity_verified(), "generation control retires undrained proof");
            commit(p, 1);
            require(!p.drain(true).empty() && p.identity_verified(), "new generation can establish fresh proof");
            event(p, control);
            require(!p.identity_verified(), "generation control retires drained proof");
        }
        std::cout << "target forensic retention PASS\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
