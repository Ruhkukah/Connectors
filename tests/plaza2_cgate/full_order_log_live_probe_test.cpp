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
    Plaza2FullOrderLog book{ids, 100, 100};
    Observer composite{window, ids, &book, 100}, reference{window, ids, nullptr, 100};
    Fixture() {
        const auto base = tables();
        schema.assign(base.begin(), base.end());
        for (const auto* name : {"heartbeat", "sys_events"})
            schema.push_back({.name = name,
                              .fields = {{.name = "replRev", .type_token = "i8", .size = 8}},
                              .index = schema.size(),
                              .row_size = 8});
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
        Fixture f;
        f.begin(f.composite);
        f.log(101, 4);
        for (std::size_t i : {std::size_t{4}, std::size_t{5}}) {
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
        std::cout
            << "PASS exact committed orders_log cutovers, LastRec, filtering, UNALIGNED, admin fences and lifecycle\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
