#include "full_order_log_test_support.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_capture.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_oi.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace full_order_log_test;
using K = Plaza2ListenerEventKind;
namespace generated = moex::plaza2::generated;
namespace fs = std::filesystem;
static Plaza2RawTableBinding deal_schema() {
    return {.name = "deal",
            .fields = {{"replID", "i8", 0, 8, 0},
                       {"replRev", "i8", 8, 8, 1},
                       {"replAct", "i8", 16, 8, 2},
                       {"sess_id", "i4", 24, 4, 3},
                       {"isin_id", "i4", 28, 4, 4},
                       {"id_deal", "i8", 32, 8, 5},
                       {"xpos", "i8", 40, 8, 6},
                       {"moment_ns", "u8", 48, 8, 7}},
            .index = 0,
            .row_size = 56};
}
static Row deal(const Plaza2RawTableBinding& t, std::int64_t rev, std::int64_t oi, std::int32_t isin = 11) {
    Row r(t);
    r.set("replID", rev);
    r.set("replRev", rev);
    r.set("sess_id", std::int32_t{321});
    r.set("isin_id", isin);
    r.set("id_deal", rev + 100);
    r.set("xpos", oi);
    r.set("moment_ns", std::uint64_t{1700000000000000000ULL} + rev);
    return r;
}
static void test_trade_export(const fs::path& root) {
    const std::array<std::int32_t, 1> ids{11};
    auto t = deal_schema();
    t.fields.insert(t.fields.end(), {{"xamount", "i8", 56, 8, 8},
                                     {"price", "d16.5", 64, 11, 9},
                                     {"moment", "t", 76, 10, 10},
                                     {"nosystem", "i1", 86, 1, 11},
                                     {"xstatus_buy", "i8", 88, 8, 12},
                                     {"xstatus_sell", "i8", 96, 8, 13}});
    t.row_size = 104;
    auto print = [&](std::int64_t rev) {
        auto r = deal(t, rev, 1000 + rev);
        r.set("xamount", std::int64_t{3});
        r.price(1234567);
        r.set("moment", moex::plaza2::public_wire::Time{2023, 11, 15, 1, 13, 20, 0});
        return r;
    };
    Plaza2FullOrderLog book(ids);
    FullOrderLogCaptureOptions options;
    options.directory = root / "trades";
    options.min_free_bytes = 0;
    FullOrderLogCapture capture(options, ids, book);
    const auto emit = [&](Plaza2ListenerEvent e) {
        e.stream_code = generated::StreamCode::kFortsDealsRepl;
        require(!capture.record(e), "capture trade event");
    };
    auto snapshot = print(1), live = print(2), unknown_side = print(3), nonsystem = print(4), interrupted = print(5);
    live.set("xstatus_buy", std::int64_t{0x20000000000});
    live.set("xstatus_sell", std::int64_t{0x40000000000});
    nonsystem.set("nosystem", std::int8_t{1});
    emit({.kind = K::LifeNum, .unsigned_value = 1});
    emit({.kind = K::Open, .raw_tables = std::span(&t, 1)});
    emit({.kind = K::TransactionBegin});
    emit(snapshot.event());
    emit({.kind = K::TransactionCommit});
    emit({.kind = K::Online});
    emit({.kind = K::TransactionBegin});
    emit(snapshot.event());
    emit(live.event());
    emit(live.event());
    emit(unknown_side.event());
    emit(nonsystem.event());
    emit({.kind = K::TransactionCommit});
    emit({.kind = K::TransactionBegin});
    emit(interrupted.event());
    emit({.kind = K::Close});
    capture.finish();

    const auto path = root / "trades.dbn";
    const auto report = export_full_order_log_dbn(options.directory, path);
    require(report.trade_prints == 2, "only unique committed online system trade prints exported");
    std::uint64_t live_commit_receipt{};
    read_full_order_log_capture(options.directory, [&](const auto& r) {
        if (r.event.kind == K::TransactionCommit)
            live_commit_receipt = r.utc_ns;
    });
    std::ifstream file(path, std::ios::binary);
    std::vector<char> data(std::istreambuf_iterator<char>(file), {});
    const auto read = [&]<class T>(std::size_t at) {
        T v;
        std::memcpy(&v, data.data() + at, sizeof v);
        return v;
    };
    std::vector<char> sides;
    for (std::size_t at = 128; at < data.size();) {
        const auto size = static_cast<unsigned char>(data[at]) * 4u;
        require(size && at + size <= data.size(), "trade DBN record size");
        if (static_cast<unsigned char>(data[at + 1]) == 0xa0 && data[at + 38] == 'T') {
            require(size == 56 && read.template operator()<std::uint32_t>(at + 4) == 11 &&
                        read.template operator()<std::uint64_t>(at + 16) == 0 &&
                        read.template operator()<std::int64_t>(at + 24) == 12345670000LL &&
                        read.template operator()<std::uint32_t>(at + 32) == 3 &&
                        (static_cast<unsigned char>(data[at + 36]) & 128) &&
                        read.template operator()<std::uint64_t>(at + 8) == 1700000000000000002ULL + sides.size() &&
                        read.template operator()<std::uint64_t>(at + 40) == live_commit_receipt,
                    "trade MBO payload, timestamp and flags");
            sides.push_back(data[at + 39]);
        }
        at += size;
    }
    require(sides == std::vector<char>{'B', 'N'}, "DBN side preserves verified aggressor and unknown");
}
int main() {
    try {
        const std::array<std::int32_t, 1> ids{11};
        auto t = deal_schema();
        FullOrderLogOpenInterest oi(ids);
        std::vector<std::int64_t> observed;
        oi.on_update = [&](const auto& v) { observed.push_back(v.quantity); };
        const auto send = [&](Plaza2ListenerEvent e) {
            e.stream_code = generated::StreamCode::kFortsDealsRepl;
            require(!oi.on_plaza2_listener_event(e), "OI callback");
        };
        const auto open = [&] { send({.kind = K::Open, .raw_tables = std::span(&t, 1)}); };
        open();
        send({.kind = K::TransactionBegin});
        auto a = deal(t, 1, 5000000000LL);
        send(a.event());
        auto other = deal(t, 2, 777, 22);
        send(other.event());
        send({.kind = K::TransactionCommit});
        require(!oi.value(11) && observed.empty(), "OI snapshot hidden until ONLINE");
        send({.kind = K::Online});
        require(oi.online() && oi.value(11)->quantity == 5000000000LL && observed.size() == 1,
                "64-bit snapshot OI at ONLINE");
        send({.kind = K::TransactionBegin});
        auto b = deal(t, 3, 5000000001LL), c = deal(t, 4, 5000000002LL);
        send(b.event());
        send(c.event());
        require(observed.size() == 1 && oi.value(11)->quantity == 5000000000LL, "OI hidden inside transaction");
        send({.kind = K::TransactionCommit});
        require(observed == std::vector<std::int64_t>{5000000000LL, 5000000001LL, 5000000002LL},
                "every live committed OI update preserved");
        send({.kind = K::ClearDeleted, .signed_value = 100, .table_index = 0, .raw_table = &t});
        require(oi.value(11)->quantity == 5000000002LL, "housekeeping preserves latest OI observation");
        send({.kind = K::TransactionBegin});
        auto interrupted = deal(t, 5, 10);
        send(interrupted.event());
        send({.kind = K::Close});
        require(!oi.online() && !oi.value(11) && observed.size() == 3, "interrupted OI transaction never exposed");
        open();
        send({.kind = K::Online});
        auto unframed = deal(t, 6, 20);
        auto event = unframed.event();
        event.stream_code = generated::StreamCode::kFortsDealsRepl;
        require(static_cast<bool>(oi.on_plaza2_listener_event(event)), "unframed online OI row refused");
        auto invalid = t;
        invalid.fields[6].type_token = "i4";
        require(static_cast<bool>(oi.on_plaza2_listener_event({.kind = K::Open,
                                                               .stream_code = generated::StreamCode::kFortsDealsRepl,
                                                               .raw_tables = std::span(&invalid, 1)})),
                "OI xpos must be i8");
        const auto root = fs::temp_directory_path() / ("fol_oi_capture_" + std::to_string(::getpid()));
        require(fs::create_directory(root), "unique OI capture root");
        auto schema = tables();
        Plaza2FullOrderLog book(ids);
        FullOrderLogCaptureOptions o;
        o.directory = root / "capture";
        o.min_free_bytes = 0;
        FullOrderLogCapture capture(o, ids, book);
        FullOrderLogOpenInterest mixed(ids);
        mixed.before_event = [&](const auto& e) { return capture.record(e); };
        require(!capture.on_plaza2_listener_event(
                    {.kind = K::Open, .stream_code = kFullOrderLogStreamCode, .raw_tables = schema}),
                "mixed FOL open");
        require(!capture.on_plaza2_listener_event({.kind = K::Online, .stream_code = kFullOrderLogStreamCode}),
                "empty valid FOL");
        const auto emit = [&](Plaza2ListenerEvent e) {
            e.stream_code = generated::StreamCode::kFortsDealsRepl;
            require(!mixed.on_plaza2_listener_event(e), "mixed OI event");
        };
        emit({.kind = K::Open, .raw_tables = std::span(&t, 1)});
        emit({.kind = K::TransactionBegin});
        emit(a.event());
        emit(other.event());
        emit({.kind = K::TransactionCommit});
        emit({.kind = K::Online});
        emit({.kind = K::TransactionBegin});
        emit(b.event());
        emit(c.event());
        emit({.kind = K::TransactionCommit});
        emit({.kind = K::Close});
        capture.finish();
        const auto replay = replay_full_order_log_capture(o.directory);
        require(replay.open_interest_rows == 3 && replay.open_interest_updates == 3 && capture.metrics().filtered == 1,
                "selected raw OI rows and committed replay");
        const auto path = root / "mixed.dbn";
        const auto exported = export_full_order_log_dbn(o.directory, path);
        require(exported.open_interest_updates == 3 && exported.trade_prints == 0, "legacy OI-only export");
        std::ifstream file(path, std::ios::binary);
        std::vector<char> data(std::istreambuf_iterator<char>(file), {});
        require(std::string_view(data.data(), 4) == std::string_view("DBN\3", 4), "DBN v3 preserves i8 statistics");
        const auto read = [&]<class T>(std::size_t at) {
            T v;
            std::memcpy(&v, data.data() + at, sizeof v);
            return v;
        };
        require(read.template operator()<std::uint16_t>(24) == UINT16_MAX, "mixed DBN schema marker");
        std::vector<std::int64_t> values;
        for (std::size_t at = 128; at < data.size();) {
            auto size = static_cast<unsigned char>(data[at]) * 4;
            require(size && (at + size <= data.size()), "mixed DBN record size");
            if (static_cast<unsigned char>(data[at + 1]) == 0x18) {
                require(size == 80 && read.template operator()<std::uint16_t>(at + 56) == 9,
                        "DBN OI statistics record");
                values.push_back(read.template operator()<std::int64_t>(at + 40));
            }
            at += size;
        }
        require(values == observed, "DBN OI values remain signed 64-bit and causal");
        test_trade_export(root);
        std::cout << "OI schema/64-bit/snapshot/commit/selection/interruption/mixed DBN checks passed at " << root
                  << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
