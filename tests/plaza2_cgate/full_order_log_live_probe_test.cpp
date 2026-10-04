#include "full_order_log_live_probe.hpp"
#include "full_order_log_test_support.hpp"
#include <iostream>
using namespace full_order_log_probe;
using namespace full_order_log_test;
int main() {
    try {
        auto schema = tables();
        const std::array<std::int32_t, 1> ids{11};
        Window window(4);
        Plaza2FullOrderLog book(ids, 100, 100);
        Observer composite(window, ids, &book, 100), reference(window, ids, nullptr, 100);
        const auto send = [](Observer& o, const Plaza2ListenerEvent& e) {
            const auto error = o.on_plaza2_listener_event(e);
            full_order_log_probe::require(!error, error.message.c_str());
        };
        const auto simple = [&](Observer& o, Plaza2ListenerEventKind kind) { send(o, {.kind = kind}); };
        const auto open = [&](Observer& o) { send(o, {.kind = Plaza2ListenerEventKind::Open, .raw_tables = schema}); };
        const auto info = [&](Observer& o, std::int64_t rev, std::int64_t life) {
            Row r(schema[1]);
            r.set("publication_state", std::int8_t{1});
            r.set("trades_rev", rev);
            r.set("trades_lifenum", life);
            send(o, r.event());
        };
        open(composite);
        open(reference);
        simple(composite, Plaza2ListenerEventKind::TransactionBegin);
        auto snapshot = row(schema[0], 101, 11, 10000000, 3, 1, 30);
        send(composite, snapshot.event());
        info(composite, 100, 7);
        simple(composite, Plaza2ListenerEventKind::Online);
        simple(composite, Plaza2ListenerEventKind::TransactionCommit);
        simple(reference, Plaza2ListenerEventKind::TransactionBegin);
        auto raw = row(schema[0], 101, 11, 10000000, 3, 1, 50);
        send(reference, raw.event());
        info(reference, 100, 7);
        simple(reference, Plaza2ListenerEventKind::TransactionCommit);
        full_order_log_probe::require(window.matched == 1 && window.mismatched == 0,
                                      "equal native revision/life must match independent raw aggregation");
        // Snapshot's own replRev=50 is deliberately unrelated to its linked trades_rev=100.
        simple(reference, Plaza2ListenerEventKind::TransactionBegin);
        info(reference, 101, 8);
        simple(reference, Plaza2ListenerEventKind::TransactionCommit);
        full_order_log_probe::require(window.matched == 1 && window.mismatched == 0,
                                      "different native life/revision must remain unaligned");
        simple(composite, Plaza2ListenerEventKind::TransactionBegin);
        auto log = row(schema[2], 101, 11, 10000000, 4, 1, 9999);
        send(composite, log.event());
        simple(composite, Plaza2ListenerEventKind::TransactionCommit);
        simple(reference, Plaza2ListenerEventKind::TransactionBegin);
        raw = row(schema[0], 101, 11, 10000000, 5, 1, 50);
        send(reference, raw.event());
        info(reference, 101, 7);
        simple(reference, Plaza2ListenerEventKind::TransactionCommit);
        full_order_log_probe::require(window.mismatched == 0 && composite.unanchored_commits == 1,
                                      "unrelated later table-local revision must not become a native snapshot anchor");
        Window mismatch;
        mismatch.record(0, {7, 100}, "quantity4");
        mismatch.record(1, {7, 100}, "quantity5");
        full_order_log_probe::require(mismatch.mismatched == 1,
                                      "same true native anchor with differing quantity must fail");
        simple(composite, Plaza2ListenerEventKind::TransactionBegin);
        send(composite, {.kind = Plaza2ListenerEventKind::ClearDeleted,
                         .signed_value = 9999,
                         .table_index = schema[2].index,
                         .raw_table = &schema[2]});
        full_order_log_probe::require(composite.p9_events == 1 && composite.p9_erased == 0,
                                      "P9 equal cutoff is a zero-erasure event");
        send(composite, {.kind = Plaza2ListenerEventKind::ClearDeleted,
                         .signed_value = 10000,
                         .table_index = schema[2].index,
                         .raw_table = &schema[2]});
        full_order_log_probe::require(composite.p9_events == 2 && composite.p9_erased == 1,
                                      "P9 log-source row below cutoff must report exact erasure");
        simple(composite, Plaza2ListenerEventKind::TransactionCommit);
        simple(composite, Plaza2ListenerEventKind::TransactionBegin);
        send(composite, {.kind = Plaza2ListenerEventKind::ClearDeleted,
                         .signed_value = INT64_MAX,
                         .table_index = schema[2].index,
                         .raw_table = &schema[2]});
        simple(composite, Plaza2ListenerEventKind::TransactionCommit);
        full_order_log_probe::require(!book.valid() && window.states[0].empty(),
                                      "MAX with no fresh snapshot must retire earlier equality/current state");
        Window bound(2);
        bound.record(0, {1, 1}, "a");
        bound.record(0, {1, 1}, "a");
        full_order_log_probe::require(bound.ambiguous == 0, "identical repeated witness must not become ambiguous");
        bound.record(0, {1, 1}, "b");
        full_order_log_probe::require(bound.ambiguous == 1,
                                      "source key reused with different state cannot prove equality");
        bound.record(0, {1, 2}, "c");
        bound.record(0, {1, 3}, "d");
        full_order_log_probe::require(bound.states[0].size() == 2 && bound.evicted == 1,
                                      "comparison history must remain bounded");
        Levels empty;
        full_order_log_probe::require(digest(empty, ids) != digest({{{11, true, 10000000}, 1}}, ids),
                                      "empty canonical digest must differ from quote state");
        std::cout
            << "PASS native revision/life alignment, raw aggregation, mismatch, bounded history and P9 diagnostics\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
