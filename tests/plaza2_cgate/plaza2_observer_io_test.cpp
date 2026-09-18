#include "../../apps/plaza2_day_observer_journal.hpp"

#include <iostream>

using namespace moex::plaza2;
using namespace observer;

int main(int argc, char** argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("expected output path and clean/crash mode");
        const bool crash = std::string_view(argv[2]) == "crash";
        Journal journal(argv[1]);
        if (std::string_view(argv[2]) == "bounds") {
            bool rejected = false;
            try {
                journal.append(std::string(Journal::record_limit, 'x'));
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            if (!rejected || journal.metrics().write_calls || journal.metrics().sync_calls)
                throw std::runtime_error("oversized record must fail before write/sync");
            journal.finish();
            return 0;
        }
        Handler handler(journal, generated::StreamCode::kFortsRefdataRepl, 1);
        using K = cgate::Plaza2ListenerEventKind;
        std::array<std::byte, 128> payload{};
        const std::array<std::byte, 10> time = {std::byte{0xea}, std::byte{7}, std::byte{9},  std::byte{18},
                                                std::byte{19},   std::byte{5}, std::byte{42}, std::byte{0},
                                                std::byte{123},  std::byte{0}};
        std::array fields = {
            cgate::Plaza2DecodedFieldValue{.field_code = generated::FieldCode::kFortsRefdataReplSessionSessId,
                                           .kind = cgate::Plaza2DecodedValueKind::SignedInteger},
            cgate::Plaza2DecodedFieldValue{.field_code = generated::FieldCode::kFortsRefdataReplSessionBegin,
                                           .kind = cgate::Plaza2DecodedValueKind::Timestamp,
                                           .unsigned_value = 1789758342,
                                           .raw_value = time}};
        auto emit = [&](K kind) {
            if (auto error =
                    handler.on_plaza2_listener_event({.kind = kind,
                                                      .table_code = generated::TableCode::kFortsRefdataReplSession,
                                                      .fields = fields,
                                                      .raw_payload = payload});
                error)
                throw std::runtime_error(error.message);
        };
        emit(K::Open);
        emit(K::TransactionBegin);
        const auto before = journal.metrics().sync_calls;
        const auto start = Journal::Stamp::now().monotonic;
        std::uint64_t max_callback_ns = 0;
        for (int i = 0; i < 50000; ++i) {
            fields[0].signed_value = i;
            payload[0] = static_cast<std::byte>(i & 255);
            const auto callback_start = Journal::Stamp::now().monotonic;
            emit(K::StreamData);
            max_callback_ns =
                std::max(max_callback_ns, static_cast<std::uint64_t>(Journal::Stamp::now().monotonic - callback_start));
        }
        const auto after_rows = journal.metrics().sync_calls;
        if (after_rows != before || journal.metrics().peak_buffer_bytes > Journal::buffer_limit)
            throw std::runtime_error("row durability or buffer bound violated");
        if (crash) {
            // Deliberately bypass destructors and buffered tail. No commit marker.
            std::cout << "{\"rows\":50000,\"row_syncs\":0,\"crash_before_commit\":true}" << std::endl;
            ::_exit(23);
        }
        emit(K::TransactionCommit);
        if (journal.metrics().sync_calls != after_rows + 1)
            throw std::runtime_error("commit must perform exactly one sync");
        emit(K::Online);
        journal.finish();
        const auto& m = journal.metrics();
        std::cout << "{\"rows\":50000,\"row_syncs\":0,\"commit_syncs\":1,\"sync_calls\":" << m.sync_calls
                  << ",\"bytes\":" << m.bytes << ",\"write_calls\":" << m.write_calls
                  << ",\"peak_buffer_bytes\":" << m.peak_buffer_bytes << ",\"max_write_ns\":" << m.max_write_ns
                  << ",\"max_sync_ns\":" << m.max_sync_ns << ",\"max_callback_ns\":" << max_callback_ns
                  << ",\"elapsed_ns\":" << Journal::Stamp::now().monotonic - start << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
