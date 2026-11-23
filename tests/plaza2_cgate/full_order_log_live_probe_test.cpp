#include "full_order_log_live_probe.hpp"
#include "full_order_log_test_support.hpp"
#include <iostream>
#include <sstream>
using namespace full_order_log_probe;
using namespace full_order_log_test;
namespace {
void expect(bool value, const char* message) {
    full_order_log_probe::require(value, message);
}
moex::plaza2::public_wire::Time wire_time(std::uint64_t utc_ns) {
    using namespace std::chrono;
    const auto local = sys_time<nanoseconds>{nanoseconds(utc_ns)} + hours(3);
    const auto date = floor<days>(local);
    const year_month_day ymd{date};
    const hh_mm_ss clock{floor<milliseconds>(local - date)};
    return {static_cast<std::uint16_t>(int(ymd.year())),           static_cast<std::uint8_t>(unsigned(ymd.month())),
            static_cast<std::uint8_t>(unsigned(ymd.day())),        static_cast<std::uint8_t>(clock.hours().count()),
            static_cast<std::uint8_t>(clock.minutes().count()),    static_cast<std::uint8_t>(clock.seconds().count()),
            static_cast<std::uint16_t>(clock.subseconds().count())};
}
struct Fixture {
    std::uint64_t source_utc{1700000000000000000ULL};
    std::vector<Plaza2RawTableBinding> schema;
    std::array<std::int32_t, 1> ids{11};
    Window window{64};
    ContentWindow content{2000, 64};
    Plaza2FullOrderLog book{ids, 100, 100};
    Observer composite{window, ids, &book, 100, nullptr, 512, &content},
        reference{window, ids, nullptr, 100, nullptr, 512, &content};
    Fixture() {
        content.utc_clock = [&] { return source_utc; };
        content.resample = [&] { composite.sample_content(); };
        const auto base = tables();
        schema.assign(base.begin(), base.end());
        open(composite);
        open(reference);
        send(composite, {.kind = Plaza2ListenerEventKind::LifeNum, .unsigned_value = 7});
        begin(composite);
        send(composite, row(schema[0], 101, 11, 10000000, 3, 1, 30).event());
        info(composite, 100, 7);
        control(composite, Plaza2ListenerEventKind::Online);
        commit(composite);
        raw(100, 3);
        expect(window.matched == 1, "native bootstrap anchor must match unrelated snapshot replRev");
    }
    void send(Observer& o, const Plaza2ListenerEvent& e) {
        const auto error = o.on_plaza2_listener_event(e);
        expect(!error, error.message.c_str());
    }
    void control(Observer& o, Plaza2ListenerEventKind kind) {
        send(o, {.kind = kind});
    }
    void open(Observer& o) {
        send(o, {.kind = Plaza2ListenerEventKind::Open, .raw_tables = schema});
    }
    void begin(Observer& o) {
        control(o, Plaza2ListenerEventKind::TransactionBegin);
    }
    void commit(Observer& o) {
        control(o, Plaza2ListenerEventKind::TransactionCommit);
    }
    void info(Observer& o, std::int64_t revision, std::int64_t life) {
        Row r(schema[1]);
        r.set("publication_state", std::int8_t{1});
        r.set("replRev", revision);
        r.set("moment", wire_time(source_utc));
        r.set("trades_rev", revision);
        r.set("trades_lifenum", life);
        send(o, r.event());
    }
    void raw(std::int64_t revision, std::int64_t quantity, std::int64_t life = 7) {
        source_utc += 3000000000ULL;
        begin(reference);
        auto r = row(schema[0], 101, 11, 10000000, quantity, 1, 50);
        r.set("moment_ns", source_utc - 1000000000ULL);
        r.set("moment", wire_time(source_utc - 1000000000ULL));
        send(reference, r.event());
        info(reference, revision, life);
        commit(reference);
    }
    void log(std::int64_t revision, std::int64_t quantity, std::int32_t isin = 11, std::int64_t status = 0x1001,
             std::int64_t act = 0) {
        auto r = row(schema[2], isin == 11 ? 101 : 202, isin, 10000000, quantity, 1, revision, status);
        r.set("replAct", act);
        send(composite, r.event());
    }
    void clear(std::int64_t revision) {
        send(composite, {.kind = Plaza2ListenerEventKind::ClearDeleted,
                         .signed_value = revision,
                         .table_index = schema[2].index,
                         .raw_table = &schema[2]});
    }
};
} // namespace
int main() {
    try {
        const std::vector<std::string> secrets{"fake-key", "fake-key-long", "fake-credential", ""};
        const std::string context = "reference listener failed; probe interrupted";
        expect(redact_probe_error(context, secrets) == context, "non-secret failure context must remain visible");
        const auto redacted =
            redact_probe_error("key=fake-key-long credential=fake-credential; fake-key-long; cause=0xA006", secrets);
        expect(redacted == "key=[REDACTED] credential=[REDACTED]; [REDACTED]; cause=0xA006",
               "loaded fake secrets, repeats and overlapping values must be redacted without hiding the cause");
        expect(redact_probe_error(context, {}) == context, "no loaded secret must not suppress diagnostics");
        {
            Fixture native;
            native.open(native.composite);
            for (const auto index : {0U, 1U})
                native.send(native.composite, {.kind = Plaza2ListenerEventKind::ClearDeleted,
                                               .signed_value = INT64_MAX,
                                               .table_index = native.schema[index].index,
                                               .raw_table = &native.schema[index]});
            for (unsigned n = 1; n <= 36; ++n)
                native.send(native.composite, row(native.schema[0], n, 33, 10000000, 2).event());
            native.send(native.composite, row(native.schema[0], 101, 11, 10000000, 3).event());
            native.begin(native.composite);
            native.info(native.composite, 100, 7);
            native.commit(native.composite);
            expect(!native.book.valid() && native.window.states[0].empty(), "probe snapshot commits stay hidden");
            native.control(native.composite, Plaza2ListenerEventKind::Online);
            expect(native.book.valid() && native.window.states[0].contains({7, 100}),
                   "probe records unframed snapshot at ONLINE with exact native info cutover");
            native.raw(100, 3);
            expect(native.window.states[0].at({7, 100}).paired,
                   "probe pairs the ONLINE snapshot with raw committed info");
            const auto error =
                native.composite.on_plaza2_listener_event(row(native.schema[2], 101, 11, 10000000, 2).event());
            expect(error && error.message.find("outside bound transaction") != std::string::npos,
                   "probe rejects unframed rows after ONLINE");
        }
        {
            Fixture empty;
            empty.open(empty.composite);
            empty.info(empty.composite, 100, 7);
            empty.control(empty.composite, Plaza2ListenerEventKind::Online);
            expect(empty.book.valid() && empty.book.order_count() == 0 && empty.window.states[0].contains({7, 100}),
                   "probe publishes a completed empty snapshot at ONLINE");
            empty.raw(100, 0);
            expect(empty.window.states[0].at({7, 100}).paired, "empty composite matches raw snapshot");
            empty.open(empty.composite);
            empty.send(empty.composite, row(empty.schema[0], 101, 11, 10000000, 3).event());
            empty.control(empty.composite, Plaza2ListenerEventKind::Close);
            expect(!empty.book.valid() && empty.window.states[0].empty(), "probe discards interrupted snapshot");
        }
        {
            Fixture internal_info;
            internal_info.open(internal_info.composite);
            internal_info.send(internal_info.composite, row(internal_info.schema[0], 101, 11, 10000000, 3).event());
            internal_info.control(internal_info.composite, Plaza2ListenerEventKind::Online);
            expect(internal_info.book.valid() && internal_info.window.states[0].empty() &&
                       internal_info.composite.unanchored_commits > 0,
                   "ONLINE completes composite without exposed info; exact comparison stays UNALIGNED");
        }
        {
            constexpr std::uint64_t base = 1700000000000000000ULL;
            expect(calendar(wire_time(base + 3000000ULL)) == "2023-11-15T01:13:20.003",
                   "calendar diagnostics must preserve millisecond precision with leading zeroes");
            constexpr std::int64_t a = 2100000000000000001LL, b = 2100000000000000007LL;
            const OrderSet old{{a, {1, 100, 7, true}}}, current{{b, {1, 100, 7, true}}};
            const auto metadata = [&](std::uint64_t snapshot, std::uint64_t received) {
                return PublicationInfo{.provided = true,
                                       .info_revision = 5,
                                       .trades_revision = 20,
                                       .log_life = 7,
                                       .end_utc_ns = snapshot,
                                       .max_order_moment_ns = snapshot - 1000000000ULL,
                                       .received_utc_ns = received};
            };
            std::ostringstream trace;
            ContentWindow stale(20, 64, &trace);
            stale.utc_clock = [&] { return base; };
            stale.change(11, a, {0, 10, 0, base - 5000000000ULL, base - 5000000000ULL, true});
            stale.change(11, b, {1, 11, 1, base - 4000000000ULL, base - 4000000000ULL, true});
            stale.record(11, current, 100);
            stale.raw(11, old, 101, 102, metadata(base - 140000000000ULL, base));
            stale.poll(123);
            expect(stale.pre_connection == 1 && stale.different == 0 && stale.stale_hypothesis_proven == 1,
                   "cached140s-old publication is not a current mismatch; every added/deleted ID proves chronology");
            expect(trace.str().find("not_comparable_pre_connection_publication") != std::string::npos &&
                       trace.str().find("proven_for_every_differing_id") != std::string::npos,
                   "pre-connection classification and causal proof are separately recorded");
            ContentWindow partial_trade(20, 64);
            partial_trade.utc_clock = [&] { return base; };
            partial_trade.change(11, a, {0, 10, 0, base - 5000000000ULL, base - 5000000000ULL, true});
            partial_trade.change(11, b, {1, 11, 2, base - 4000000000ULL, base - 4000000000ULL, true});
            partial_trade.change(11, b, {2, 12, 1, base - 3000000000ULL, base - 3000000000ULL, true});
            partial_trade.record(11, current, 100);
            partial_trade.raw(11, old, 101, 102, metadata(base - 140000000000ULL, base));
            partial_trade.poll(123);
            expect(partial_trade.stale_hypothesis_proven == 1 && !partial_trade.suspected_defects,
                   "a partial trade after the retained Add preserves its proof of post-snapshot creation");
            ContentWindow conflicting_delete(20, 64);
            conflicting_delete.utc_clock = [&] { return base; };
            conflicting_delete.change(11, a, {0, 10, 0, base - 5000000000ULL, base - 5000000000ULL, true});
            conflicting_delete.change(11, b, {1, 11, 1, base - 4000000000ULL, base - 4000000000ULL, true});
            conflicting_delete.change(11, b, {0, 12, 0, base - 3000000000ULL, base - 3000000000ULL, true});
            conflicting_delete.record(11, current, 100);
            conflicting_delete.raw(11, old, 101, 102, metadata(base - 140000000000ULL, base));
            conflicting_delete.poll(123);
            expect(conflicting_delete.suspected_defects == 1 && !conflicting_delete.stale_hypothesis_proven,
                   "a later committed delete contradicts a composite state that still contains the ID");
            ContentWindow uncertain_transition(20, 64);
            uncertain_transition.utc_clock = [&] { return base; };
            uncertain_transition.change(11, a, {0, 10, 0, base - 5000000000ULL, base - 5000000000ULL, true});
            uncertain_transition.change(11, b, {1, 11, 1, base - 150000000000ULL, base - 150000000000ULL, true});
            uncertain_transition.change(11, b, {1, 12, 1, 0, 0, false});
            uncertain_transition.record(11, current, 100);
            uncertain_transition.raw(11, old, 101, 102, metadata(base - 140000000000ULL, base));
            uncertain_transition.poll(123);
            expect(!uncertain_transition.stale_hypothesis_proven && !uncertain_transition.suspected_defects,
                   "an unknown later transition cannot be ignored to claim either proof or contrary chronology");
            // A later publication cannot reuse the initial exclusion, even with a regressed timestamp.
            stale.raw(11, old, 200, 201, metadata(base - 140000000000ULL, base));
            stale.poll(222);
            expect(stale.different == 1 && stale.pre_connection == 1,
                   "later mismatches never disappear behind an old/regressed publication time");
            ContentWindow wrong(20, 64);
            wrong.utc_clock = [&] { return base; };
            wrong.change(11, a, {0, 10, 0, base - 150000000000ULL, base - 150000000000ULL, true});
            wrong.change(11, b, {1, 11, 1, base - 4000000000ULL, base - 4000000000ULL, true});
            wrong.record(11, current, 100);
            wrong.raw(11, old, 101, 102, metadata(base - 140000000000ULL, base));
            wrong.poll(123);
            expect(wrong.suspected_defects == 1 && wrong.stale_hypothesis_proven == 0,
                   "a missing ID deleted before the snapshot contradicts the proposed stale-snapshot explanation");
            ContentWindow unknown(20, 64);
            unknown.utc_clock = [&] { return base; };
            unknown.record(11, current, 100);
            unknown.raw(11, old, 101, 102, metadata(base - 140000000000ULL, base));
            unknown.poll(123);
            expect(unknown.pre_connection == 1 && unknown.stale_hypothesis_proven == 0,
                   "publication predates history but absent lifecycle evidence cannot prove the hypothesis");
            ContentWindow absent(20, 64);
            absent.utc_clock = [&] { return base; };
            absent.record(11, current, 100);
            auto no_time = metadata(0, base);
            no_time.max_order_moment_ns = 0;
            absent.raw(11, old, 101, 102, no_time);
            absent.poll(123);
            expect(absent.unknown_publication == 1 && absent.pre_connection == 0 && absent.stale_hypothesis_proven == 0,
                   "missing publication time is explicitly unknown, never assumed stale");
            ContentWindow future(20, 64);
            future.utc_clock = [&] { return base; };
            future.record(11, current, 100);
            future.raw(11, old, 101, 102, metadata(base + 5000000000ULL, base));
            future.poll(123);
            expect(future.unknown_publication == 1 && future.stale_hypothesis_proven == 0,
                   "future/inconsistent timestamps cannot qualify a publication");
            ContentWindow quiet(20, 64);
            std::uint64_t now = base;
            quiet.utc_clock = [&] { return now; };
            quiet.record(11, current, 100);
            for (int n = 0; n != 3; ++n) {
                now = base + (n + 1) * 180000000000ULL;
                auto info = metadata(now - 10000000ULL, now);
                info.info_revision += n;
                info.max_order_moment_ns = base - 100000000000ULL; // Quiet resting orders are old, info is fresh.
                quiet.raw(11, current, 200 + n * 100, 201 + n * 100, info);
            }
            expect(quiet.fresh_equal == 3 && !quiet.fresh_check_pass(599) && quiet.fresh_check_pass(600),
                   "three fresh quiet publications qualify only after10min, without requiring recent order activity");
            quiet.retire();
            expect(!quiet.fresh_check_pass(600) && quiet.fresh_counts.empty(),
                   "retiring a generation invalidates old fresh-publication counts even with no pending observations");
            quiet.record(11, current, 550);
            expect(!quiet.fresh_check_pass(600), "a new composite alone cannot reuse earlier raw-publication proof");
            for (int n = 0; n != 3; ++n) {
                now = base + (n + 4) * 180000000000ULL;
                auto info = metadata(now - 10000000ULL, now);
                info.info_revision = 8 + n;
                quiet.raw(11, current, 560 + n * 10, 561 + n * 10, info);
            }
            expect(quiet.fresh_check_pass(600), "three new fresh publications qualify the new generation");
            quiet.raw(11, current, 600, 601, no_time);
            expect(
                quiet.unknown_later_publication == 1 && !quiet.fresh_check_pass(600),
                "a later publication with unknown time still compares but cannot silently qualify among fresh equals");
            ContentWindow multiple(20, 64);
            multiple.utc_clock = [&] { return base; };
            for (const auto isin : {11, 12, 13}) {
                multiple.record(isin, current, 100);
                multiple.raw(isin, current, 200, 201, metadata(base + 5000000000ULL, base + 5000000000ULL));
            }
            expect(multiple.fresh_equal == 3 && !multiple.fresh_check_pass(600),
                   "one publication for each of three instruments is not three publications per instrument");
            for (const auto duplicate_kind : {0, 1, 2}) {
                ContentWindow repeated(20, 64);
                repeated.utc_clock = [&] { return base; };
                repeated.record(11, current, 100);
                for (int n = 0; n != 3; ++n) {
                    auto info =
                        metadata(base + (duplicate_kind == 2 ? 10 + n : 10) * 1000000000ULL, base + 30000000000ULL);
                    if (duplicate_kind == 1)
                        info.info_revision += n; // Timestamp does not advance.
                    repeated.raw(11, current, 200 + n * 100, 201 + n * 100, info);
                }
                expect(
                    repeated.fresh_equal == 1 && repeated.matched == 3 && repeated.unknown_later_publication == 2 &&
                        !repeated.fresh_check_pass(600),
                    "duplicate identity, repeated time, or repeated revision never creates three fresh publications");
            }
            ContentWindow regressed(20, 64);
            regressed.utc_clock = [&] { return base; };
            regressed.record(11, current, 100);
            regressed.raw(11, current, 200, 201, metadata(base + 10000000000ULL, base + 30000000000ULL));
            auto earlier = metadata(base + 5000000000ULL, base + 30000000000ULL);
            earlier.info_revision = 6;
            regressed.raw(11, old, 300, 301, earlier);
            regressed.poll(322);
            expect(regressed.fresh_observations == 1 && regressed.different == 1 &&
                       regressed.unknown_later_publication == 1 && !regressed.fresh_check_pass(600),
                   "a later regressing snapshot remains a mismatch even when its time is after the first composite");
            // Later committed events do not belong to the nearest earlier state, even within clock tolerance.
            ContentWindow later(20, 64);
            later.utc_clock = [&] { return base; };
            later.record(11, current, 100);
            later.change(11, a, {0, 10, 0, base - 5000000000ULL, base - 5000000000ULL, true});
            later.change(11, b, {1, 11, 1, base - 4000000000ULL, base - 4000000000ULL, true});
            later.raw(11, old, 101, 102, metadata(base - 140000000000ULL, base));
            later.poll(123);
            expect(later.stale_hypothesis_proven == 0 && later.suspected_defects == 0,
                   "events assigned to a later unrecorded commit cannot prove or contradict the compared state");
            ContentWindow evicted(20, 64, nullptr, 2);
            evicted.utc_clock = [&] { return base; };
            evicted.change(11, a, {0, 10, 0, base - 5000000000ULL, base - 5000000000ULL, true});
            evicted.change(11, b, {1, 11, 1, base - 4000000000ULL, base - 4000000000ULL, true});
            evicted.change(11, b, {1, 12, 1, base - 3000000000ULL, base - 3000000000ULL, true});
            evicted.record(11, current, 100);
            evicted.raw(11, old, 101, 102, metadata(base - 140000000000ULL, base));
            evicted.poll(123);
            expect(evicted.change_evictions == 1 && evicted.stale_hypothesis_proven == 0,
                   "bounded lifecycle eviction explicitly prevents a causal proof");
            ContentWindow fresh_bad(20, 64);
            std::uint64_t received = base;
            fresh_bad.utc_clock = [&] { return received; };
            fresh_bad.record(11, current, 100);
            received += 5000000000ULL;
            fresh_bad.raw(11, old, 101, 102, metadata(received - 10000000ULL, received));
            fresh_bad.poll(123);
            expect(fresh_bad.different == 1 && fresh_bad.fresh_different == 1 && !fresh_bad.fresh_check_pass(600),
                   "a genuinely fresh publication's mismatch remains a failure");
            auto untrusted = metadata(base - 140000000000ULL, base);
            untrusted.timestamp_consistent = false;
            ContentWindow wrong_timezone(20, 64);
            wrong_timezone.utc_clock = [&] { return base; };
            wrong_timezone.record(11, current, 100);
            wrong_timezone.raw(11, old, 101, 102, untrusted);
            wrong_timezone.poll(123);
            expect(wrong_timezone.unknown_publication == 1 && wrong_timezone.pre_connection == 0,
                   "a raw calendar/UTC disagreement cannot establish stale or fresh authority");
            wrong_timezone.raw(11, old, 200, 201, untrusted);
            wrong_timezone.poll(222);
            expect(wrong_timezone.different == 1, "later mismatches cannot be discarded for unknown time");
            ContentWindow raw_first(20, 64);
            raw_first.utc_clock = [&] { return base; };
            raw_first.raw(11, old, 90, 91, metadata(base - 140000000000ULL, base));
            raw_first.poll(120);
            expect(raw_first.pending.size() == 1, "raw-before-composite bootstrap awaits a real first committed image");
            raw_first.record(11, current, 121);
            raw_first.poll(122);
            expect(raw_first.pre_connection == 1, "first composite time is established after early raw data");
        }
        {
            // Real T0 shape: unframed snapshot, framed log catch-up before ONLINE, then cached raw publication.
            constexpr std::uint64_t now = 1700000000000000000ULL;
            constexpr std::int64_t old_id = 2100000000000000001LL, new_id = 2100000000000000007LL;
            const auto schema = tables();
            const std::array<std::int32_t, 1> ids{11};
            Window window;
            ContentWindow content(20, 64);
            content.utc_clock = [] { return now; };
            Plaza2FullOrderLog book(ids);
            Observer composite(window, ids, &book, 100, nullptr, 512, &content);
            Observer reference(window, ids, nullptr, 100, nullptr, 512, &content);
            content.resample = [&] { composite.sample_content(); };
            const auto send = [&](Observer& target, const Plaza2ListenerEvent& event) {
                const auto error = target.on_plaza2_listener_event(event);
                expect(!error, error.message.c_str());
            };
            for (auto* target : {&composite, &reference})
                send(*target, {.kind = Plaza2ListenerEventKind::Open, .raw_tables = schema});
            send(composite, row(schema[0], old_id, 11, 100, 1).event());
            send(composite, {.kind = Plaza2ListenerEventKind::TransactionBegin});
            for (const auto id : {old_id, new_id}) {
                auto log =
                    row(schema[2], id, 11, 100, id == old_id ? 0 : 1, id == old_id ? 0 : 1, id == old_id ? 100 : 101);
                const auto moment = now - (id == old_id ? 5000000000ULL : 4000000000ULL);
                log.set("moment_ns", moment);
                log.set("moment", wire_time(moment));
                send(composite, log.event());
            }
            send(composite, {.kind = Plaza2ListenerEventKind::TransactionCommit});
            expect(content.changes.empty() && !book.valid(), "pre-ONLINE catch-up stays hidden and untrusted");
            send(composite, {.kind = Plaza2ListenerEventKind::Online});
            expect(book.valid() && content.changes.size() == 2,
                   "successful ONLINE commits all pre-ONLINE lifecycle evidence");
            send(reference, {.kind = Plaza2ListenerEventKind::TransactionBegin});
            auto order = row(schema[0], old_id, 11, 100, 1);
            order.set("moment_ns", now - 150000000000ULL);
            order.set("moment", wire_time(now - 150000000000ULL));
            send(reference, order.event());
            Row info(schema[1]);
            info.set("publication_state", std::int8_t{1});
            info.set("replRev", std::int64_t{500});
            info.set("trades_rev", std::int64_t{20});
            info.set("trades_lifenum", std::int64_t{7});
            info.set("moment", wire_time(now - 140000000000ULL));
            send(reference, info.event());
            send(reference, {.kind = Plaza2ListenerEventKind::TransactionCommit});
            content.poll(ContentWindow::now() + 21);
            expect(
                content.pre_connection == 1 && content.stale_hypothesis_proven == 1 && !content.different,
                "actual pre-ONLINE add/delete catch-up proves every stale raw-set difference after its snapshot time");
            send(composite, {.kind = Plaza2ListenerEventKind::Close});
            expect(content.changes.empty(), "Close retires diagnostic lifecycle authority");
        }
        {
            ContentWindow w(20, 8);
            w.record(11, "before", 100);
            w.raw(11, "after", 105, 110);
            expect(w.matched == 0 && w.pending.size() == 1, "in-flight publication waits for tolerance");
            w.record(11, "after", 125);
            expect(w.matched == 1 && w.pending.empty(), "later committed content inside tolerance aligns");
            w.raw(11, "wrong", 200, 205);
            w.poll(226);
            expect(w.different == 1, "persistent content difference is reported after grace, not hidden");
            w.raw(22, "after", 230, 235);
            w.poll(256);
            expect(w.unresolved == 1, "a different ISIN must not match");
            w.raw(11, "after", 300, 305);
            expect(w.matched == 2, "quiet committed books remain comparable");
            w.retire();
            w.raw(11, "after", 310, 315);
            w.poll(336);
            expect(w.unresolved == 2, "retired lifecycle state cannot match a new publication");
            ContentWindow bounded(20, 1);
            bounded.record(11, "old", 10);
            bounded.raw(11, "missing", 11, 12);
            bounded.record(11, "new", 13);
            expect(bounded.unresolved == 1 && bounded.different == 0 && bounded.evictions == 1,
                   "history eviction cannot be reported as a definite difference");
            bounded.raw(11, "lost", 11, 14);
            bounded.poll(35);
            expect(bounded.unresolved == 2 && bounded.different == 0,
                   "a publication arriving after history loss remains unresolved");
            OrderSet a{{1, {2, 100, 7, true}}}, b{{2, {2, 100, 7, true}}};
            expect(order_set_digest(a) != order_set_digest(b), "equal levels with different IDs cannot match");
            b = a;
            b.at(1) = {1, 100, 7, true};
            expect(order_set_digest(a) != order_set_digest(b), "remaining quantity participates in content");
        }
        {
            constexpr std::int64_t a = 9000000000000000111LL, b = 9000000000000000222LL;
            constexpr std::int64_t c = 9000000000000000333LL, d = 9000000000000000444LL;
            const OrderSet nearest{{a, {1, 987654321099999LL, 7, true}},
                                   {b, {2, 123456789099999LL, 7, true}},
                                   {c, {3, 777777777099999LL, 7, false}}};
            OrderSet raw = nearest;
            raw.at(a) = {9, 987654321099999LL, 7, true};
            raw.erase(c);
            raw.emplace(d, std::tuple{4LL, 555555555099999LL, 7, false});
            std::ostringstream trace;
            ContentWindow diagnostics(20, 8, &trace);
            diagnostics.record(11, OrderSet{{a, {8, 1, 7, true}}}, 100);
            diagnostics.record(11, nearest, 120);
            diagnostics.record(11, nearest, 125, false); // Idle inspection is not another native commit.
            diagnostics.raw(11, raw, 110, 115);
            expect(trace.str().empty(), "no-match diagnostics wait for the arrival grace");
            diagnostics.record(11, nearest, 140); // Outside expanded window:90..135.
            const auto text = trace.str();
            expect(diagnostics.different == 1 && text.find("\"missing_orders\":1") != std::string::npos &&
                       text.find("\"extra_orders\":1") != std::string::npos &&
                       text.find("\"changed_orders\":1") != std::string::npos &&
                       text.find("\"composite_commits_in_window\":2") != std::string::npos,
                   "no-match must report symmetric differences against nearest state and only in-window commits");
            for (auto id : {a, c, d})
                expect(text.find("\"order_id\":\"" + std::to_string(id) + "\"") != std::string::npos,
                       "every differing order ID must be retained losslessly above2^53");
            expect(text.find(std::to_string(b)) == std::string::npos,
                   "unchanged order IDs are not symmetric-difference evidence");
            diagnostics.raw(22, raw, 200, 205);
            diagnostics.poll(226);
            expect(trace.str().find("\"missing_orders\":null") != std::string::npos,
                   "missing nearest state must report unavailable counts, not false zeros");
            ContentWindow bounded_sets(20, 8, nullptr, 3);
            bounded_sets.record(11, nearest, 100);
            bounded_sets.raw(11, OrderSet{}, 101, 102);
            bounded_sets.record(11, OrderSet{{d, {4, 1, 7, true}}}, 103);
            expect(*bounded_sets.retained_rows <= 3 && bounded_sets.unresolved == 1 && bounded_sets.evictions == 1,
                   "order-image storage limit must retire pending observations explicitly");
        }
        {
            Fixture no_info;
            no_info.open(no_info.composite);
            const auto before_online = no_info.content.matched;
            no_info.send(no_info.composite, row(no_info.schema[0], 101, 11, 10000000, 3).event());
            no_info.control(no_info.composite, Plaza2ListenerEventKind::Online);
            no_info.raw(200, 3);
            expect(no_info.content.matched == before_online + 1,
                   "actual no-info ONLINE snapshot compares by order content");
            const auto matches = no_info.content.matched;
            no_info.begin(no_info.composite);
            no_info.log(201, 2);
            bool visited = false;
            expect(!no_info.book.inspect_committed_orders([&](const auto&) { visited = true; }) && !visited,
                   "diagnostic inspection never leaks a partially applied transaction");
            no_info.raw(201, 2);
            expect(no_info.content.matched == matches, "uncommitted content does not match");
            no_info.commit(no_info.composite);
            expect(no_info.content.matched == matches + 1, "committed update compares without composite info");
        }
        {
            Fixture dirty;
            dirty.send(dirty.composite, {.kind = Plaza2ListenerEventKind::ClearDeleted,
                                         .signed_value = 31,
                                         .table_index = dirty.schema[0].index,
                                         .raw_table = &dirty.schema[0]});
            bool visited = false;
            expect(!dirty.book.inspect_committed_orders([&](const auto&) { visited = true; }) && !visited,
                   "out-of-frame administrative changes stay hidden until the next commit");
            dirty.begin(dirty.composite);
            dirty.commit(dirty.composite);
            const auto before = dirty.content.matched;
            dirty.content.retire();
            dirty.raw(500, 0);
            expect(dirty.content.matched == before + 1,
                   "raw publication can resample a quiet valid committed empty book after cohort retirement");
        }
        Fixture f;
        f.begin(f.composite);
        f.log(101, 4);
        for (std::size_t i : {std::size_t{3}}) {
            Row unrelated(f.schema[i]);
            unrelated.set("replRev", std::int64_t{999999});
            f.send(f.composite, unrelated.event());
        }
        f.commit(f.composite);
        f.raw(101, 4);
        expect(f.window.matched == 2 && !f.window.states[0].contains({7, 999999}),
               "unrelated table revisions cannot replace orders_log cutover");
        f.begin(f.composite);
        f.log(102, 5, 11, 1);
        f.log(103, 9, 22);
        expect(!f.window.states[0].contains({7, 103}), "LastRec before TN_COMMIT is not a committed witness");
        f.commit(f.composite);
        f.raw(103, 5);
        expect(f.window.matched == 3, "filtered ISIN LastRec must advance whole orders_log table cutover");
        f.begin(f.composite);
        f.log(104, 6);
        f.log(105, 7);
        f.commit(f.composite);
        f.raw(104, 6);
        expect(!f.window.states[0].contains({7, 104}) && !f.window.states[1].at({7, 104}).paired,
               "interior matching boundary without CGate commit stays UNALIGNED");
        f.raw(105, 7);
        expect(f.window.matched == 4, "final exact committed cutover must compare");
        f.begin(f.composite);
        f.log(106, 8, 11, 1);
        f.commit(f.composite);
        expect(!f.window.states[0].contains({7, 106}), "incomplete matching transaction cannot become a cutover");
        f.begin(f.composite);
        f.log(107, 9);
        f.commit(f.composite);
        f.raw(107, 9);
        expect(f.window.matched == 5, "later completed transaction restores eligible commit");
        f.begin(f.composite);
        f.clear(107);
        expect(f.composite.p9_events == 1 && f.composite.p9_erased == 0, "equal cutoff erases zero log rows");
        f.clear(108);
        f.log(108, 10);
        f.commit(f.composite);
        expect(f.composite.p9_events == 2 && f.composite.p9_erased == 1 && f.window.states[0].at({7, 107}).ambiguous &&
                   !f.window.states[0].contains({7, 108}),
               "clear ambiguity is fenced even when a business revision advances in the same block");
        f.begin(f.composite);
        f.clear(INT64_MAX);
        f.commit(f.composite);
        expect(!f.book.valid() && f.window.states[0].empty() && f.window.states[1].empty(),
               "MAX without fresh snapshot retires both generations/current history");
        f.open(f.composite);
        f.begin(f.composite);
        f.send(f.composite, row(f.schema[0], 101, 11, 10000000, 2, 1, 30).event());
        f.info(f.composite, 100, 7);
        f.control(f.composite, Plaza2ListenerEventKind::Online);
        f.commit(f.composite);
        expect(f.window.mismatched == 0 && !f.window.states[0].at({7, 100}).paired,
               "fresh same-life reused revision cannot pair with pre-MAX raw state");
        Fixture administrative;
        administrative.begin(administrative.composite);
        administrative.log(101, 0, 11, 0x1001, 100);
        administrative.commit(administrative.composite);
        expect(!administrative.window.states[0].contains({7, 101}),
               "newer administrative replAct with inherited LastRec cannot prove a matching cutover");
        Fixture stale_info;
        stale_info.begin(stale_info.composite);
        stale_info.log(101, 4);
        stale_info.info(stale_info.composite, 100, 7);
        stale_info.commit(stale_info.composite);
        expect(!stale_info.window.states[0].contains({7, 101}) && stale_info.window.composite_commits == 1 &&
                   stale_info.window.matched == 1 && stale_info.window.ambiguous == 0,
               "old snapshot info cannot relabel or mutate the existing committed anchor");
        Fixture incomplete_info;
        incomplete_info.begin(incomplete_info.composite);
        incomplete_info.log(101, 4, 11, 1);
        incomplete_info.info(incomplete_info.composite, 101, 7);
        incomplete_info.commit(incomplete_info.composite);
        expect(!incomplete_info.window.states[0].contains({7, 101}),
               "snapshot info cannot complete a non-LastRec online row");
        Fixture lifecycle;
        lifecycle.send(lifecycle.composite, {.kind = Plaza2ListenerEventKind::LifeNum, .unsigned_value = 8});
        expect(lifecycle.window.states[0].empty() && lifecycle.window.states[1].empty() && !lifecycle.book.valid(),
               "LifeNum retires both comparison histories");
        lifecycle.control(lifecycle.reference, Plaza2ListenerEventKind::LifeNum);
        expect(lifecycle.window.states[1].empty(), "raw LifeNum retires reference history");
        Fixture peer_reset;
        peer_reset.control(peer_reset.reference, Plaza2ListenerEventKind::LifeNum);
        peer_reset.raw(100, 2);
        peer_reset.begin(peer_reset.composite);
        peer_reset.commit(peer_reset.composite);
        expect(peer_reset.window.states[0].empty() && peer_reset.window.mismatched == 0 &&
                   !peer_reset.window.states[1].at({7, 100}).paired,
               "empty peer commit cannot re-emit old qty3 against fresh same-life/revision raw qty2");
        peer_reset.begin(peer_reset.composite);
        peer_reset.log(101, 4);
        peer_reset.commit(peer_reset.composite);
        peer_reset.raw(101, 4);
        expect(peer_reset.window.states[0].at({7, 101}).paired, "fresh source progress and raw publication can pair");
        Fixture reopen;
        reopen.open(reopen.composite);
        expect(reopen.window.states[0].empty() && reopen.window.states[1].empty(),
               "OPEN retires both prior native-generation witness histories");
        Window mismatch;
        mismatch.record(0, {7, 100}, "quantity4");
        mismatch.record(1, {7, 100}, "quantity5");
        expect(mismatch.mismatched == 1, "same exact committed anchor with different quotes fails");
        mismatch.record(0, {7, 100}, "quantity6");
        expect(mismatch.mismatched == 0 && mismatch.ambiguous == 1 && !mismatch.states[1].at({7, 100}).paired,
               "disqualified mismatched pair must withdraw the failure count symmetrically");
        Window evicted_peer(1);
        evicted_peer.record(0, {7, 100}, "A");
        evicted_peer.record(1, {7, 100}, "B");
        evicted_peer.record(1, {7, 101}, "C");
        evicted_peer.record(0, {7, 100}, "D");
        expect(evicted_peer.mismatched == 0 && evicted_peer.ambiguous == 1 &&
                   !evicted_peer.states[0].at({7, 100}).paired,
               "disqualification withdraws remembered pair outcome even after peer eviction");
        Window bound(2);
        bound.record(0, {1, 1}, "a");
        bound.record(1, {1, 1}, "a");
        bound.record(0, {1, 1}, "b");
        expect(bound.ambiguous == 1 && bound.matched == 0 && !bound.states[1].at({1, 1}).paired,
               "same key with changed digest must withdraw earlier pairing");
        bound.record(0, {1, 2}, "c");
        bound.record(0, {1, 3}, "d");
        expect(bound.states[0].size() == 2 && bound.evicted == 1, "comparison history is bounded");
        expect(digest({}, f.ids) != digest({{{11, true, 10000000}, 1}}, f.ids), "empty digest is distinct");
        std::cout << "PASS order content arrival windows, no-info ONLINE, exact cutovers, filtering and lifecycle\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
