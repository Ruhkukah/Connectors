#include "full_order_log_live_probe.hpp"
#include "full_order_log_test_support.hpp"
#include <iostream>
using namespace full_order_log_probe;
using namespace full_order_log_test;
namespace {
void expect(bool value, const char* message) {
    full_order_log_probe::require(value, message);
}
struct Fixture {
    std::vector<Plaza2RawTableBinding> schema;
    std::array<std::int32_t, 1> ids{11};
    Window window{64};
    ContentWindow content{2000, 64};
    Plaza2FullOrderLog book{ids, 100, 100};
    Observer composite{window, ids, &book, 100, nullptr, 512, &content},
        reference{window, ids, nullptr, 100, nullptr, 512, &content};
    Fixture() {
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
        r.set("trades_rev", revision);
        r.set("trades_lifenum", life);
        send(o, r.event());
    }
    void raw(std::int64_t revision, std::int64_t quantity, std::int64_t life = 7) {
        begin(reference);
        send(reference, row(schema[0], 101, 11, 10000000, quantity, 1, 50).event());
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
