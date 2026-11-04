#include "full_order_log_test_support.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_capture.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace full_order_log_test;
namespace fs = std::filesystem;
using Kind = Plaza2ListenerEventKind;
static bool refused(const auto& action) {
    try {
        action();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}
static auto bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(f), {});
}
int main() {
    try {
        const auto root = fs::temp_directory_path() / ("fol_capture_" + std::to_string(::getpid()));
        require(fs::create_directory(root), "unique capture test root");
        const auto schema = tables();
        std::array<std::int32_t, 1> ids{11};
        Plaza2FullOrderLog book(ids, 1024, 256);
        FullOrderLogCaptureOptions o;
        o.directory = root / "good";
        o.block_bytes = 4096;
        o.queue_blocks = 32;
        o.segment_bytes = 8192;
        o.max_bytes = 4 << 20;
        o.min_free_bytes = 0;
        o.flush_interval = std::chrono::milliseconds(1);
        o.sync_interval = std::chrono::milliseconds(1);
        std::vector<std::vector<std::byte>> expected;
        std::vector<std::vector<std::uint8_t>> nulls;
        FullOrderLogCapture capture(o, ids, book);
        const auto send = [&](Plaza2ListenerEvent e) {
            e.stream_code = kFullOrderLogStreamCode;
            require(!capture.on_plaza2_listener_event(e), "capture callback");
        };
        send({.kind = Kind::Open, .raw_tables = schema});
        const auto selected = [&](const Row& r) {
            expected.emplace_back(r.event().raw_payload.begin(), r.event().raw_payload.end());
            nulls.push_back(r.nulls);
            send(r.event());
        };
        selected(row(schema[0], 1, 11, 1234567, 10)); // Unframed snapshot stays hidden.
        send(row(schema[0], 999, 22, 1234567, 10).event());
        // An excluded add is still recorded in full before book filtering.
        selected(row(schema[0], 2, 11, 1234567, 5, 1, 2, 0x2));
        require(!book.valid(), "snapshot hidden while recording");
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        capture.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        require(capture.metrics().stored_bytes > 32, "quiet partial block flush reaches disk worker");
        send({.kind = Kind::Online});
        require(book.valid(), "snapshot commits at ONLINE");
        for (int i = 0; i < 1000; ++i) {
            send({.kind = Kind::TransactionBegin});
            selected(row(schema[2], 1, 11, 1234567, 3 + i % 2, 1, 10 + i));
            send({.kind = Kind::TransactionCommit});
            if (i % 5 == 0) {
                capture.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        send({.kind = Kind::Close});
        capture.finish();
        const auto m = capture.metrics();
        require(!m.failed && m.filtered == 1 && m.syncs > 0, "capture drained and synced without drops");
        std::size_t seen{};
        auto summary = read_full_order_log_capture(o.directory, [&](const auto& r) {
            if (r.event.kind == Kind::StreamData) {
                require(seen < expected.size() && std::equal(r.event.raw_payload.begin(), r.event.raw_payload.end(),
                                                             expected[seen].begin(), expected[seen].end()),
                        "full row bytes preserved");
                require(std::equal(r.event.raw_nulls.begin(), r.event.raw_nulls.end(), nulls[seen].begin(),
                                   nulls[seen].end()),
                        "null maps preserved");
                ++seen;
            }
        });
        require(seen == expected.size(), "all selected and excluded rows present");
        const auto replay = replay_full_order_log_capture(o.directory);
        require(replay.valid_commits == 1001 && replay.last_valid_order_count == 1,
                "replay uses exact book rules and committed visibility");
        const auto dbn = root / "orders.dbn";
        const auto exported = export_full_order_log_dbn(o.directory, dbn);
        require(exported.valid_commits == 1001, "DBN export committed points");
        auto data = bytes(dbn);
        require(data.size() > 128 && std::string_view(data.data(), 4) == std::string_view("DBN\3", 4) &&
                    (data.size() - 128) % 56 == 0,
                "DBN framing");
        require(refused([&] { export_full_order_log_dbn(o.directory, dbn); }), "export refuses overwrite");
        // Check independent wire semantics: initial snapshot, absolute M quantities, C removes, R withdraws.
        std::map<std::uint64_t, std::uint32_t> orders;
        bool modify{};
        std::uint32_t sequence{};
        const auto scalar = [&]<class T>(std::size_t at) {
            T v;
            std::memcpy(&v, data.data() + at, sizeof v);
            return v;
        };
        for (std::size_t at = 128; at < data.size(); at += 56) {
            require(static_cast<unsigned char>(data[at]) == 14 && static_cast<unsigned char>(data[at + 1]) == 0xa0 &&
                        scalar.template operator()<std::uint32_t>(at + 52) == ++sequence,
                    "DBN record sequence");
            const auto id = scalar.template operator()<std::uint64_t>(at + 16);
            const auto size = scalar.template operator()<std::uint32_t>(at + 32);
            const auto action = data[at + 38];
            if (action == 'R')
                orders.clear();
            else if (action == 'A')
                orders[id] = size;
            else if (action == 'M') {
                require(orders.contains(id), "modify known order");
                orders[id] = size;
                modify = true;
            } else if (action == 'C')
                orders.erase(id);
            else
                require(false, "no fabricated trade events");
        }
        require(modify && orders.empty(), "DBN quantities and close withdrawal");
        for (const auto mode : {"truncated", "corrupted", "missing"}) {
            auto dest = root / mode;
            fs::copy(o.directory, dest, fs::copy_options::recursive);
            std::vector<fs::path> segments;
            for (const auto& p : fs::directory_iterator(dest))
                if (p.path().extension() == ".folz")
                    segments.push_back(p.path());
            std::sort(segments.begin(), segments.end());
            require(segments.size() > 1, "rotation exercised");
            if (std::string_view(mode) == "missing")
                fs::remove(segments.front());
            else if (std::string_view(mode) == "truncated")
                fs::resize_file(segments.back(), fs::file_size(segments.back()) - 1);
            else {
                std::fstream file(segments.front(), std::ios::in | std::ios::out | std::ios::binary);
                file.seekp(100);
                file.put('\xff');
            }
            require(refused([&] { replay_full_order_log_capture(dest); }), "damaged capture rejected");
            require(refused([&] { export_full_order_log_dbn(dest, root / (std::string(mode) + ".dbn")); }) &&
                        !fs::exists(root / (std::string(mode) + ".dbn")),
                    "damaged export not published");
        }
        {
            auto recover = o;
            recover.directory = root / "reopen";
            Plaza2FullOrderLog b(ids);
            FullOrderLogCapture c(recover, ids, b);
            const auto emit = [&](Plaza2ListenerEvent e) {
                e.stream_code = kFullOrderLogStreamCode;
                require(!c.on_plaza2_listener_event(e), "recovery callback");
            };
            emit({.kind = Kind::Open, .raw_tables = schema});
            emit(row(schema[0], 1, 11, 1234567, 10).event());
            emit({.kind = Kind::Online});
            c.on_plaza2_listener_error({.code = Plaza2ErrorCode::CallbackFailed,
                                        .runtime_code = 0xA006,
                                        .message = "offline listener timeout"});
            emit({.kind = Kind::Open, .raw_tables = schema});
            emit(row(schema[0], 2, 11, 1234567, 7).event());
            emit({.kind = Kind::Online});
            c.finish();
            require(refused([&] { replay_full_order_log_capture(recover.directory); }), "strict error marker refused");
            const auto replay = replay_full_order_log_capture(recover.directory, true);
            require(replay.truncated && replay.listener_errors == 1 && replay.valid_commits == 2 &&
                        replay.last_valid_order_count == 1 && replay.last_good_sequence > 0,
                    "explicit recovery replays fresh snapshot after error");
            const auto out = root / "reopen.dbn";
            export_full_order_log_dbn(recover.directory, out, true);
            const auto v = bytes(out);
            unsigned snapshots{}, bad_resets{};
            for (std::size_t at = 128; at < v.size(); at += 56) {
                if (v[at + 38] == 'R' && (static_cast<unsigned char>(v[at + 36]) & 4))
                    ++bad_resets;
                if (v[at + 38] == 'A' && (static_cast<unsigned char>(v[at + 36]) & 32))
                    ++snapshots;
            }
            require(snapshots == 2 && bad_resets >= 1, "error withdraws DBN then exports fresh snapshot");
        }
        {
            // A complete nonzero host stop carries status, rather than masquerading as a crash.
            auto bad = o;
            bad.directory = root / "host-nonzero";
            Plaza2FullOrderLog b(ids);
            FullOrderLogCapture c(bad, ids, b);
            require(!c.on_plaza2_listener_event(
                        {.kind = Kind::Open, .stream_code = kFullOrderLogStreamCode, .raw_tables = schema}),
                    "nonzero OPEN");
            c.finish(FullOrderLogCaptureEndStatus::HostNonzero, 7);
            const auto result = read_full_order_log_capture(bad.directory, [](const auto&) {}, true);
            require(result.truncated && result.host_status == 7 &&
                        result.end_status == FullOrderLogCaptureEndStatus::HostNonzero,
                    "nonzero status preserved");
            require(refused([&] { replay_full_order_log_capture(bad.directory); }), "strict rejects host-nonzero");
        }
        {
            // Stop at the verified chunk boundary; no staged half-transaction is exported.
            auto cut = o;
            cut.directory = root / "prefix";
            Plaza2FullOrderLog b(ids);
            {
                FullOrderLogCapture c(cut, ids, b);
                const auto emit = [&](Plaza2ListenerEvent e) {
                    e.stream_code = kFullOrderLogStreamCode;
                    require(!c.on_plaza2_listener_event(e), "prefix callback");
                };
                emit({.kind = Kind::Open, .raw_tables = schema});
                emit(row(schema[0], 1, 11, 1234567, 10).event());
                emit({.kind = Kind::Online});
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                c.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                emit({.kind = Kind::TransactionBegin});
                emit(row(schema[2], 1, 11, 1234567, 3, 1, 100).event());
                // Destructor drains complete bytes, but deliberately no clean end/commit.
            }
            require(refused([&] { replay_full_order_log_capture(cut.directory); }), "strict missing end");
            const auto summary = replay_full_order_log_capture(cut.directory, true);
            require(summary.truncated && summary.valid_commits == 1 && summary.last_good_sequence > 1,
                    "incomplete replay exposes committed prefix only");
            const auto out = root / "prefix.dbn";
            export_full_order_log_dbn(cut.directory, out, true);
            const auto data = bytes(out);
            require(data[data.size() - 56 + 38] == 'R' && (static_cast<unsigned char>(data[data.size() - 56 + 36]) & 4),
                    "truncated export withdraws book");
            std::vector<fs::path> segments;
            for (const auto& p : fs::directory_iterator(cut.directory))
                if (p.path().extension() == ".folz")
                    segments.push_back(p.path());
            std::sort(segments.begin(), segments.end());
            fs::resize_file(segments.back(), fs::file_size(segments.back()) - 1);
            const auto recovered = replay_full_order_log_capture(cut.directory, true);
            require(recovered.truncated && recovered.last_good_sequence < summary.last_good_sequence &&
                        recovered.valid_commits == 1,
                    "damaged final chunk entirely ignored");
        }
        {
            auto bad = o;
            bad.directory = root / "invalid-row";
            Plaza2FullOrderLog b(ids);
            FullOrderLogCapture c(bad, ids, b);
            require(!c.on_plaza2_listener_event(
                        {.kind = Kind::Open, .stream_code = kFullOrderLogStreamCode, .raw_tables = schema}),
                    "bad fixture open");
            require(!c.on_plaza2_listener_event({.kind = Kind::Online, .stream_code = kFullOrderLogStreamCode}),
                    "empty ONLINE");
            auto bad_row = row(schema[2], 1, 11, 100, 1);
            auto r = bad_row.event();
            require(static_cast<bool>(c.on_plaza2_listener_event(r)), "unframed post-ONLINE row refused");
            c.finish();
            require(refused([&] { replay_full_order_log_capture(bad.directory); }),
                    "validation-failed capture not passed off as valid replay");
            require(refused([&] { replay_full_order_log_capture(bad.directory, true); }),
                    "allow-incomplete does not relax row validation");
        }
        {
            auto bad = o;
            bad.directory = root / "aborted";
            {
                Plaza2FullOrderLog b(ids);
                FullOrderLogCapture c(bad, ids, b);
                require(!c.on_plaza2_listener_event(
                            {.kind = Kind::Open, .stream_code = kFullOrderLogStreamCode, .raw_tables = schema}),
                        "aborted open");
            }
            require(refused([&] { replay_full_order_log_capture(bad.directory); }), "missing clean end refused");
        }
        {
            auto bad = o;
            bad.directory = root / "space";
            bad.min_free_bytes = UINT64_MAX;
            Plaza2FullOrderLog b(ids);
            require(refused([&] { FullOrderLogCapture c(bad, ids, b); }), "disk reserve refused before host start");
        }

        // Exercise final partial-block publication while the worker is starting/idle.
        for (unsigned trial = 0; trial < 100; ++trial) {
            auto small = o;
            small.directory = root / ("close-" + std::to_string(trial));
            small.queue_blocks = 2;
            FullOrderLogCapture closing(small, ids, book);
            require(!closing.on_plaza2_listener_event(
                        {.kind = Kind::Open, .stream_code = kFullOrderLogStreamCode, .raw_tables = schema}),
                    "close-race OPEN");
            require(!closing.on_plaza2_listener_event({.kind = Kind::Online, .stream_code = kFullOrderLogStreamCode}),
                    "close-race ONLINE");
            require(!closing.on_plaza2_listener_event({.kind = Kind::Close, .stream_code = kFullOrderLogStreamCode}),
                    "close-race Close");
            std::this_thread::yield();
            closing.finish();
            const auto checked = read_full_order_log_capture(small.directory, [](const auto&) {});
            require(checked.records == 3 && closing.metrics().records == 5, "final block including end marker drained");
        }
        require(refused([&] { FullOrderLogCapture duplicate(o, ids, book); }), "cannot overwrite existing capture");
        std::cout << "capture bytes/filter/quiet flush/rotation/sync/replay/DBN/corruption/failure checks passed at "
                  << root << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
