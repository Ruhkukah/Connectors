#include "full_order_log_test_support.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_backfill.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_capture.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_refdata.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <tuple>
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
static void test_backfill(const fs::path& root, FullOrderLogCaptureOptions options) {
    const auto schema = tables();
    const std::array<std::int32_t, 1> ids{11};
    Plaza2FullOrderLog book(ids, 1024, 256);
    const auto record = [](FullOrderLogCapture& capture, Plaza2ListenerEvent e) {
        e.stream_code = kFullOrderLogStreamCode;
        require(!capture.record(e), "backfill fixture recorder");
    };
    const auto frame = [&](FullOrderLogCapture& capture, std::int64_t rev, std::int64_t qty = 3) {
        record(capture, {.kind = Kind::TransactionBegin});
        record(capture, row(schema[2], 1, 11, 1234565, qty, 1, rev).event());
        record(capture, {.kind = Kind::TransactionCommit});
    };
    constexpr std::string_view composite = "p2ordbook://FORTS_ORDLOG_REPL_MATCH2;snapshot=FORTS_ORDBOOK_REPL_MATCH2";
    const auto primary_fixture = [&](std::string_view name, std::string_view service, int left_life, int right_life,
                                     bool notify_after_online = false) {
        const auto directory = root / name;
        options.directory = directory;
        FullOrderLogCapture capture(options, ids, book);
        const auto open = [&](std::int64_t life) {
            record(capture, {.kind = Kind::Open, .text_value = service, .raw_tables = schema});
            if (life) {
                Row info(schema[1]);
                info.set("publication_state", std::int8_t{1});
                info.set("trades_lifenum", life);
                record(capture, info.event());
            }
            record(capture, {.kind = Kind::Online});
        };
        open(left_life);
        frame(capture, 101);
        frame(capture, 104);
        record(capture, {.kind = Kind::TransactionBegin});
        record(capture, row(schema[2], 1, 11, 1234565, 3, 1, 105).event());
        capture.record_error(kFullOrderLogStreamCode, {.code = Plaza2ErrorCode::RuntimeCallFailed});
        open(right_life);
        if (notify_after_online)
            record(capture, {.kind = Kind::LifeNum, .unsigned_value = 7});
        frame(capture, 111);
        record(capture, {.kind = Kind::Close});
        capture.finish();
        return directory;
    };
    const auto original = primary_fixture("backfill-original", composite, 7, 7);
    const auto plan = plan_full_order_log_backfill(original, 2);
    require(plan.listener_settings == "p2repl://FORTS_ORDLOG_REPL_MATCH2;tables=orders_log" &&
                plan.open_settings == "mode=snapshot+online;lifenum=7;rev.orders_log=101" && plan.ordlog_lifenum == 7 &&
                plan.start.revision == 101 && plan.left.revision == 104 && plan.right.revision == 111 &&
                plan.left.commit_sequence < plan.reopen_sequence && plan.reopen_sequence < plan.right.commit_sequence,
            "plan derives generation and uses earlier durable commit, excluding interrupted frame");
    require(refused([&] { plan_full_order_log_backfill(original, 2, 8); }),
            "supplied life conflicts with recorded info");
    require(refused([&] { plan_full_order_log_backfill(original, 1); }),
            "supplied matching conflicts with recorded services");
    for (const auto [name, left, right, notify] : {std::tuple{"no-left-life", 0, 7, false},
                                                   {"no-right-life", 7, 0, false},
                                                   {"changed-life", 7, 8, false},
                                                   {"online-life", 7, 7, true}}) {
        const auto invalid = primary_fixture(name, composite, left, right, notify);
        require(refused([&] { plan_full_order_log_backfill(invalid, 2); }),
                "missing, conflicting or invalidated composite generation refused");
    }
    const auto unsuffixed =
        primary_fixture("unsuffixed", "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL", 7, 7);
    require(refused([&] { plan_full_order_log_backfill(unsuffixed, 2); }), "legacy unsuffixed archive refused");
    const auto unproven = primary_fixture("unproven", {}, 7, 7);
    require(refused([&] { plan_full_order_log_backfill(unproven, 2); }), "unproven legacy archive refused");
    const auto primary = read_full_order_log_capture(original, [](const auto&) {}, true);
    require(primary.truncated && primary.fol_coverage.opens == 2 && primary.fol_coverage.listener_errors == 1 &&
                primary.deals_coverage.opens == 0 && primary.deals_coverage.listener_errors == 0,
            "FOL coverage remains incomplete independently from DEALS");
    for (const auto kind : {"good", "wrong-life", "different-row", "missing-right", "missing-left", "collector",
                            "wrong-request", "collector-stale"}) {
        options.directory = root / (std::string("backfill-") + kind);
        const std::string_view mode(kind);
        const bool collector = mode == "collector" || mode == "wrong-request" || mode == "collector-stale";
        options.recovery_cursor.reset();
        if (collector)
            options.recovery_cursor =
                FullOrderLogRecoveryCursor{plan.capture_identity, plan.reopen_sequence, plan.ordlog_lifenum,
                                           plan.start.revision, mode == "wrong-request" ? 3 : 2};
        {
            FullOrderLogCapture capture(options, ids, book);
            if (mode == "collector-stale")
                record(capture, {.kind = Kind::LifeNum, .unsigned_value = 8u});
            record(capture, {.kind = Kind::Open, .raw_tables = std::span(schema).subspan(2, 1)});
            if (!collector)
                record(capture, {.kind = Kind::LifeNum, .unsigned_value = mode == "wrong-life" ? 8u : 7u});
            if (mode != "missing-left")
                frame(capture, 104, mode == "different-row" ? 4 : 3);
            frame(capture, 105);
            if (mode != "missing-right")
                frame(capture, 111);
            if (!collector) {
                record(capture, {.kind = Kind::Online});
                record(capture, {.kind = Kind::Close});
            }
            capture.finish();
        }
        const auto check = [&] {
            return check_full_order_log_backfill(original, options.directory, 2, 7, plan.reopen_sequence);
        };
        if (mode == "good" || mode == "collector") {
            const auto result = check();
            require(result.recovery_capture_identity != plan.capture_identity && result.committed_rows_between == 1,
                    "distinct clean capture overlaps both committed boundaries before ONLINE");
            const auto evidence_only = [](const auto& fn) {
                try {
                    fn();
                    return false;
                } catch (const std::exception& error) {
                    return std::string_view(error.what()).find("use check-backfill") != std::string_view::npos;
                }
            };
            require(evidence_only([&] { replay_full_order_log_capture(options.directory); }),
                    "recovery evidence has explicit replay refusal");
            const auto output = root / "recovery.dbn";
            require(evidence_only([&] { export_full_order_log_dbn(options.directory, output); }) && !fs::exists(output),
                    "recovery evidence has explicit export refusal");
        } else
            require(refused(check), "unproved overlap refused");
    }
}
static void test_refdata_join(const fs::path& root, FullOrderLogCaptureOptions options) {
    namespace g = moex::plaza2::generated;
    const auto schema = tables();
    const std::array<std::int32_t, 2> ids{11, 22};
    Plaza2FullOrderLog book(ids, 1024, 256);
    options.directory = root / "refdata";
    FullOrderLogCapture capture(options, ids, book);
    FullOrderLogRefdataValue first{
        .isin_id = 11,
        .available = true,
        .isin = "TEST-FUT-11",
        .short_code = "F11",
        .min_step = "0.00005",
        .decimals = 5,
        .sess_id = 321,
        .instrument_source = {g::StreamCode::kFortsRefdataRepl, g::TableCode::kFortsRefdataReplFutSessContents, 100, 7,
                              true},
        .session_source = {g::StreamCode::kFortsRefdataRepl, g::TableCode::kFortsRefdataReplSession, 50, 7, true},
    };
    capture.record_refdata(first);
    const auto emit = [&](Plaza2ListenerEvent event) {
        event.stream_code = kFullOrderLogStreamCode;
        require(!capture.on_plaza2_listener_event(event), "record REFDATA join fixture");
    };
    emit({.kind = Kind::Open, .raw_tables = schema});
    emit(row(schema[0], 1, 11, 1234565, 10, 1, 1).event());
    emit(row(schema[0], 2, 22, 1234500, 5, 1, 2).event());
    emit({.kind = Kind::Online});
    first.instrument_source.repl_rev = 101;
    first.min_step = "0.00010";
    capture.record_refdata(first);
    auto second = first;
    second.isin_id = 22;
    second.isin = "TEST-FUT-22";
    second.short_code = "F22";
    second.instrument_source.repl_rev = 102;
    capture.record_refdata(second);
    emit({.kind = Kind::TransactionBegin});
    emit(row(schema[2], 1, 11, 1234565, 3, 1, 3).event());
    emit({.kind = Kind::TransactionCommit});
    emit({.kind = Kind::Close});
    capture.finish();

    const auto dbn = root / "refdata.dbn";
    const auto report = export_full_order_log_dbn(options.directory, dbn);
    const auto tsv_bytes = bytes(options.directory / kFullOrderLogRefdataFilename);
    const std::string tsv(tsv_bytes.begin(), tsv_bytes.end());
    const auto rows = parse_full_order_log_refdata(tsv);
    require(rows.size() == 3 && rows[0].min_step == "0.00005" && rows[1].min_step == "0.00010" &&
                rows[1].effective_after_sequence == rows[2].effective_after_sequence,
            "sidecar retains metadata revisions and sequence ties");
    require(report.refdata_sha256 == plaza2_sha256_hex(tsv) && report.dbn_sha256 == plaza2_sha256_file(dbn) &&
                report.refdata_unjoined_records == 3,
            "strict export pins DBN and capture sidecar and counts the uncovered prefix");
    const auto companion_bytes = bytes(dbn.string() + ".refdata.json");
    const std::string companion(companion_bytes.begin(), companion_bytes.end());
    require(companion.find("\"dbn_sha256\":\"" + report.dbn_sha256 + "\"") != std::string::npos &&
                companion.find("\"capture_refdata_sha256\":\"" + report.refdata_sha256 + "\"") != std::string::npos &&
                companion.find("\"dbn_instrument_ids\":[11,22]") != std::string::npos &&
                companion.find("\"effective_after_dbn_sequence\":[0,6,6]") != std::string::npos &&
                companion.find("\"refdata_tsv\":" + text::json_quote_utf8(tsv)) != std::string::npos,
            "export companion carries exact source sidecar and causal DBN boundaries");
    const std::array<std::uint32_t, 3> effective_after{0, 6, 6};
    const auto data = bytes(dbn);
    std::size_t unjoined{}, joined{};
    for (std::size_t at = 128; at < data.size(); at += 56) {
        require(data.size() - at >= 56 && data[at] == 14, "join fixture record framing");
        std::uint32_t instrument{}, sequence{};
        std::memcpy(&instrument, data.data() + at + 4, sizeof instrument);
        std::memcpy(&sequence, data.data() + at + 52, sizeof sequence);
        const FullOrderLogRefdataRow* metadata = nullptr;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (static_cast<std::uint32_t>(rows[i].isin_id) == instrument && sequence > effective_after[i])
                metadata = &rows[i];
        if (metadata) {
            require(metadata->isin == (instrument == 11 ? "TEST-FUT-11" : "TEST-FUT-22") && metadata->sess_id == 321 &&
                        metadata->instrument_source.present,
                    "DBN instrument joins the effective instrument and session provenance");
            ++joined;
        } else {
            require(instrument == 22 && sequence <= 6, "only startup rows can lack current REFDATA in this fixture");
            ++unjoined;
        }
    }
    require(joined > 0 && unjoined == report.refdata_unjoined_records, "as-of join never fills the earlier prefix");
    const auto tampered = root / "refdata-tampered";
    fs::copy(options.directory, tampered, fs::copy_options::recursive);
    std::ofstream(tampered / kFullOrderLogRefdataFilename, std::ios::app) << "tamper\n";
    const auto bad_dbn = root / "refdata-tampered.dbn";
    require(refused([&] { export_full_order_log_dbn(tampered, bad_dbn, true); }) && !fs::exists(bad_dbn) &&
                !fs::exists(bad_dbn.string() + ".refdata.json"),
            "even explicit incomplete recovery refuses a broken REFDATA pin before publication");
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
        require(exported.refdata_sha256.empty() && !fs::exists(dbn.string() + ".refdata.json"),
                "legacy capture exports without an invented sidecar");
        test_refdata_join(root, o);
        test_backfill(root, o);
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
                        replay.last_valid_order_count == 1 && replay.last_good_sequence > 0 &&
                        replay.end_status == FullOrderLogCaptureEndStatus::Clean && replay.host_status == 0,
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
