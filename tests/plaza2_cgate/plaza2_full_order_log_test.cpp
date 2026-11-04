#include "full_order_log_test_support.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <random>
#include <thread>

static std::atomic<std::size_t> allocations{};
static bool count_allocations{};
void* operator new(std::size_t bytes) {
    if (count_allocations)
        ++allocations;
    if (auto* p = std::malloc(bytes ? bytes : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
using namespace full_order_log_test;
using Kind = Plaza2ListenerEventKind;

static void semantics() {
    Harness h;
    for (const auto kind : {Kind::TransactionBegin, Kind::TransactionCommit, Kind::ReplState, Kind::Timeout})
        require(!h.book.should_log_listener_event({.kind = kind}), "routine callback controls cannot flood logs");
    const Plaza2RawTableBinding sys{.name = "sys_events"};
    require(h.book.should_log_listener_event({.kind = Kind::ClearDeleted}) &&
                h.book.should_log_listener_event({.kind = Kind::StreamData, .raw_table = &sys}) &&
                !h.book.should_log_listener_event({.kind = Kind::StreamData, .raw_table = &h.schema[2]}),
            "sparse diagnostics preserve clear/system events and suppress public order rows");
    std::size_t commits{};
    h.book.on_commit = [&](const auto&) { ++commits; };
    h.begin();
    h.apply(row(h.schema[0], 1, 11, 1234567, 10));
    h.commit();
    require(!h.book.valid() && commits == 0, "snapshot remains hidden before ONLINE");
    h.online();
    require(!h.book.valid(), "ONLINE alone does not publish");
    h.begin();
    h.commit();
    require(h.book.valid() && commits == 1 && h.book.levels(11, true).at(1234567) == 10,
            "online commit publishes snapshot");
    const auto other_revision = h.book.revision(22), own_revision = h.book.revision(11);
    h.begin();
    h.add(1, 11, 1234567, 6, 2, 2);
    require(h.book.levels(11, true).at(1234567) == 10, "uncommitted remainder is invisible");
    h.commit();
    require(h.book.levels(11, true).at(1234567) == 6, "partial trade replaces direct remainder");
    require(h.book.revision(11) == own_revision + 1, "one committed instrument revision");
    require(h.book.revision(22) == other_revision, "revision is per instrument");
    h.begin();
    h.add(1, 11, 1234567, 0, 2, 3);
    h.commit();
    require(h.book.order_count() == 0 && h.book.levels(11, true).empty(), "full trade removes order");
    h.begin();
    h.add(2, 11, -1234567, 8);
    h.add(3, 11, -1234567, 2);
    h.commit();
    require(h.book.levels(11, true).at(-1234567) == 10, "signed exact prices aggregate");
    h.begin();
    h.add(2, 11, -1234567, 8, 0, 4);
    h.commit();
    require(h.book.levels(11, true).at(-1234567) == 2, "cancel removes regardless of nonzero cancel remainder");
    h.begin();
    auto erased = row(h.schema[2], 3, 11, 0, 0, 1, 5);
    erased.set("replID", std::int64_t{1});
    erased.set("replAct", std::int64_t{1});
    erased.nulls[erased.field("price").index] = 1;
    h.apply(erased);
    h.commit();
    require(h.book.order_count() == 0, "replAct erases before nonessential decoding");
    h.begin();
    h.add(4, 11, 100000, 7, 1, 6, 0x2);
    h.add(4, 11, 100000, 2, 2, 7, 0x2);
    h.add(5, 11, 100000, 7, 1, 8, 0x4);
    h.add(6, 11, 100000, 7, 1, 9, 0x8000000);
    h.add(7, 11, 100000, 10, 1, 10);
    h.add(7, 11, 100000, 4, 2, 11, 0x2);
    h.commit();
    require(h.book.order_count() == 1 && h.book.levels(11, true).at(100000) == 4,
            "IOC remainders and NonQuote excluded; resting IOC-flagged execution applied");
    h.begin();
    const auto before = h.book.metrics();
    auto excluded = row(h.schema[2], 9, 33, 0, 1);
    excluded.nulls[excluded.field("price").index] = 1;
    excluded.nulls[excluded.field("sess_id").index] = 1;
    h.apply(excluded);
    h.commit();
    require(h.book.order_count() == 1 && h.book.metrics().rows_total == before.rows_total + 1 &&
                h.book.metrics().rows_filtered == before.rows_filtered + 1,
            "ISIN filter precedes other field validation and filtered rows enter total rate");
}

static void committed_source_session() {
    Harness h;
    h.begin();
    h.add(1, 11, 100000, 4);
    h.commit();
    require(h.book.committed_session_id(11) == 0, "pre-ONLINE source session is not entry authority");
    h.online();
    h.begin();
    require(h.book.committed_session_id(11) == 0, "ONLINE alone cannot publish session authority");
    h.commit();
    require(h.book.committed_session_id(11) == 321 && h.book.committed_session_id(22) == 0 &&
                h.book.committed_session_id(33) == 0,
            "only a committed nonempty configured book has a session");
    h.begin();
    auto other = row(h.schema[2], 2, 22, 100000, 2);
    other.set("sess_id", std::int32_t{322});
    h.apply(other);
    require(h.book.committed_session_id(22) == 0, "uncommitted rows cannot publish a session");
    h.commit();
    require(h.book.committed_session_id(22) == 322 && h.book.committed_session_id(11) == 321,
            "source sessions are per instrument");
    h.begin();
    auto mixed = row(h.schema[2], 3, 11, 100000, 3);
    mixed.set("sess_id", std::int32_t{322});
    h.apply(mixed);
    require(h.book.committed_session_id(11) == 321, "working mixed session is invisible until commit");
    h.commit();
    require(h.book.valid() && h.book.committed_session_id(11) == 0 && h.book.levels(11, true).at(100000) == 7,
            "mixed-session retained orders cannot be retagged as current authority");
    h.begin();
    h.add(1, 11, 100000, 0, 0, 2);
    require(h.book.committed_session_id(11) == 0, "working old-session deletion cannot publish authority");
    h.commit();
    require(h.book.committed_session_id(11) == 322 && h.book.levels(11, true).at(100000) == 3,
            "removing the last old-session order recovers the retained new-session authority at commit");
    h.clear(0, std::numeric_limits<std::int64_t>::max());
    require(h.book.committed_session_id(11) == 0 && h.book.committed_session_id(22) == 0,
            "MAX immediately withdraws committed source sessions");
    h.open();
    h.online();
    h.begin();
    h.apply(mixed);
    h.commit();
    require(h.book.committed_session_id(11) == 322, "fresh epoch can establish a new committed source session");
    h.begin();
    auto depleted = row(h.schema[2], 3, 11, 100000, 0, 0, 3);
    depleted.set("sess_id", std::int32_t{322});
    h.apply(depleted);
    require(h.book.committed_session_id(11) == 322, "pending depletion preserves prior committed authority");
    h.commit();
    require(h.book.committed_session_id(11) == 0, "committed empty book has no source session authority");
    h.begin();
    h.add(7, 11, 100000, 2);
    require(h.book.committed_session_id(11) == 0, "new-session arrival remains hidden before commit");
    h.commit();
    require(h.book.committed_session_id(11) == 321, "empty-to-new-session rollover needs no reset");
    h.open();
    h.online();
    h.begin();
    auto unknown = row(h.schema[2], 4, 11, 100000, 1);
    unknown.set("sess_id", std::int32_t{0});
    h.apply(unknown);
    h.commit();
    require(h.book.valid() && h.book.committed_session_id(11) == 0,
            "unknown admitted source session withholds authority without fabricating a session");
    h.begin();
    h.add(5, 11, 100000, 2);
    h.commit();
    require(h.book.committed_session_id(11) == 0, "later known sessions cannot retag unknown retained orders");
    h.begin();
    h.add(4, 11, 100000, 0, 2, 2);
    h.commit();
    require(h.book.committed_session_id(11) == 321, "removing the unknown-session order recovers known authority");
    h.book.reset();
    require(h.book.committed_session_id(11) == 0, "reset withdraws source session authority");
    h.open();
    h.online();
    h.begin();
    h.add(6, 11, 100000, 2);
    h.commit();
    require(h.book.committed_session_id(11) == 321, "fresh epoch restores source session proof");
    require(!h.book.on_plaza2_listener_event({.kind = Kind::LifeNum, .unsigned_value = 7}) &&
                !h.book.on_plaza2_listener_event({.kind = Kind::LifeNum, .unsigned_value = 99}) &&
                h.book.committed_session_id(11) == 0,
            "unexplained LifeNum clears committed source session proof");
    auto incompatible = tables();
    incompatible[0].fields[4].type_token = "i8";
    require(h.book.on_plaza2_listener_event({.kind = Kind::Open, .raw_tables = incompatible}).code ==
                Plaza2ErrorCode::IncompatibleScheme,
            "sess_id must negotiate exact i4");
}

static void session_rollover_deletions() {
    for (const auto method : {0, 1, 2, 3}) {
        Harness h;
        h.online();
        h.begin();
        h.apply(row(h.schema[0], 1, 11, 100000, 4, 1, 10));
        h.add(2, 11, 200000, 5, 1, 11, 1, 2);
        auto fresh = row(h.schema[2], 3, 11, 100000, 3, 1, 12);
        fresh.set("sess_id", std::int32_t{322});
        h.apply(fresh);
        h.commit();
        require(h.book.committed_session_id(11) == 0, "mixed retained session generations have no authority");
        h.begin();
        if (method == 2)
            h.clear(0, 11);
        else
            h.add(1, 11, 100000, 0, method == 1 ? 2 : 0, 20);
        h.commit();
        require(h.book.committed_session_id(11) == 0 && h.book.order_count() == 2,
                "one remaining old-session order still blocks new-session authority");
        h.begin();
        if (method == 2)
            h.clear(2, 12); // Equality retains the new-session row at revision12.
        else {
            auto removed = row(h.schema[2], 2, 11, 200000, 0, method == 1 ? 2 : 0, 21, 1, 2);
            if (method == 3) {
                removed.set("replID", std::int64_t{11});
                removed.set("replAct", std::int64_t{1});
                removed.nulls[removed.field("sess_id").index] = 1;
            }
            h.apply(removed);
        }
        require(h.book.committed_session_id(11) == 0, "session-count changes remain uncommitted");
        h.commit();
        require(h.book.committed_session_id(11) == 322 && h.book.order_count() == 1,
                "cancel, full trade, finite clear and replAct removals recover the sole retained session");
    }
    Harness changed;
    changed.online();
    changed.begin();
    changed.add(1, 11, 100000, 4);
    changed.commit();
    changed.begin();
    auto relabel = row(changed.schema[2], 1, 11, 100000, 3, 2, 2);
    relabel.set("sess_id", std::int32_t{322});
    changed.apply(relabel);
    changed.commit();
    require(changed.book.committed_session_id(11) == 0 && changed.book.order_count() == 1,
            "a contradictory update cannot retag a retained old-session order as new");
    changed.begin();
    changed.add(1, 11, 100000, 0, 0, 3);
    relabel.set("public_order_id", std::int64_t{2});
    relabel.set("replID", std::int64_t{4});
    relabel.set("replRev", std::int64_t{4});
    relabel.set("public_action", std::int8_t{1});
    changed.apply(relabel);
    changed.commit();
    require(changed.book.committed_session_id(11) == 322,
            "deleting the contradictory old order and adding a new identity restores new-session authority");
}

static void recovery() {
    Harness h;
    h.online();
    h.begin();
    h.apply(row(h.schema[0], 1, 11, 100000, 2, 1, 1));
    h.add(2, 11, 200000, 3, 1, 2);
    h.add(3, 22, 300000, 4, 1, 3);
    h.commit();
    h.clear(2, 3);
    require(h.book.order_count() == 3 && h.book.levels(11, true).contains(200000), "ClearDeleted waits for TN_COMMIT");
    h.begin();
    h.commit();
    require(h.book.order_count() == 2 && h.book.levels(11, true).contains(100000),
            "ClearDeleted strict revision and table scope");
    h.clear(2, std::numeric_limits<std::int64_t>::max());
    h.begin();
    h.commit();
    require(h.book.order_count() == 0 && !h.book.valid(),
            "ClearDeleted MAX resets the composite and awaits a fresh snapshot");
    require(!h.book.on_plaza2_listener_event({.kind = Kind::LifeNum, .unsigned_value = 1}), "first life");
    const auto epoch = h.book.epoch();
    require(!h.book.on_plaza2_listener_event({.kind = Kind::LifeNum, .unsigned_value = 2}), "new life");
    require(!h.book.valid() && !h.book.order_count() && h.book.epoch() > epoch, "LifeNum change invalidates all state");
    h.begin();
    h.apply(row(h.schema[0], 1, 11, 400000, 5));
    h.commit();
    h.online();
    h.begin();
    h.commit();
    require(h.book.valid() && h.book.levels(11, true).at(400000) == 5, "new life fresh snapshot");
    h.control(Kind::Close);
    h.open();
    h.begin();
    h.apply(row(h.schema[0], 1, 11, 500000, 6));
    h.commit();
    h.online();
    h.begin();
    h.commit();
    require(h.book.order_count() == 1 && h.book.levels(11, true).at(500000) == 6, "fresh reopen drops old snapshot");
    h.begin();
    auto malformed = row(h.schema[2], 9, 11, 1, 2);
    malformed.nulls[malformed.field("price").index] = 1;
    require(static_cast<bool>(h.book.on_plaza2_listener_event(malformed.event())), "null required price fails");
    require(!h.book.valid() && h.book.levels(11, true).empty(), "callback failure invalidates committed generation");
    auto missing = tables();
    missing[2].name = "other";
    const auto error = h.book.on_plaza2_listener_event({.kind = Kind::Open, .raw_tables = missing});
    require(error.code == Plaza2ErrorCode::IncompatibleScheme, "missing table is permanent schema incompatibility");
    missing = tables();
    missing[3].name = "multileg_orders_log";
    require(h.book.on_plaza2_listener_event({.kind = Kind::Open, .raw_tables = missing}).code ==
                Plaza2ErrorCode::IncompatibleScheme,
            "sys_events is mandatory");
    std::vector<Plaza2RawTableBinding> extra(missing.begin(), missing.end());
    extra[3].name = "sys_events";
    extra.push_back({.name = "heartbeat", .index = 4});
    require(h.book.on_plaza2_listener_event({.kind = Kind::Open, .raw_tables = extra}).code ==
                Plaza2ErrorCode::IncompatibleScheme,
            "extra composite table violates restricted subscription");
    Harness small(2, 8);
    small.online();
    small.begin();
    small.add(1, 11, 1, 1);
    small.add(2, 11, 2, 1);
    require(static_cast<bool>(small.book.on_plaza2_listener_event(row(small.schema[2], 3, 11, 3, 1).event())),
            "capacity is bounded");
    require(!small.book.valid() && !small.book.order_count(), "capacity exhaustion invalidates book");
}

static void independent_snapshot() {
    struct TruthOrder {
        std::int32_t isin;
        std::int64_t price, qty;
        std::int8_t dir;
    };
    std::map<std::int64_t, TruthOrder> truth;
    Harness log(4096, 512);
    log.online();
    log.begin();
    std::mt19937_64 random(20261004);
    std::int64_t revision{};
    for (int n = 0; n < 10000; ++n) {
        const auto id = static_cast<std::int64_t>(random() % 1000 + 1);
        auto i = truth.find(id);
        if (i == truth.end()) {
            TruthOrder o{id % 2 ? 11 : 22, static_cast<std::int64_t>(random() % 64 - 32) * 100000,
                         static_cast<std::int64_t>(random() % 20 + 1), static_cast<std::int8_t>(random() % 2 + 1)};
            truth.emplace(id, o);
            log.add(id, o.isin, o.price, o.qty, 1, ++revision, 1, o.dir);
        } else {
            auto& o = i->second;
            if (random() % 3 == 0) {
                log.add(id, o.isin, o.price, o.qty, 0, ++revision, 1, o.dir);
                truth.erase(i);
            } else {
                const auto rest = static_cast<std::int64_t>(random() % (o.qty + 1));
                log.add(id, o.isin, o.price, rest, 2, ++revision, 1, o.dir);
                if (!rest)
                    truth.erase(i);
                else
                    o.qty = rest;
            }
        }
        if (n % 50 == 49) {
            log.commit();
            log.begin();
        }
    }
    log.commit();
    Harness snapshot(4096, 512);
    snapshot.begin();
    for (const auto& [id, o] : truth)
        snapshot.apply(row(snapshot.schema[0], id, o.isin, o.price, o.qty, 1, revision, 1, o.dir));
    snapshot.commit();
    snapshot.online();
    snapshot.begin();
    snapshot.commit();
    for (auto isin : log.ids)
        for (auto bid : {true, false}) {
            std::map<std::int64_t, std::int64_t> expected;
            for (const auto& [id, o] : truth)
                if (o.isin == isin && (o.dir == 1) == bid)
                    expected[o.price] += o.qty;
            const auto& actual = log.book.levels(isin, bid);
            const auto& replayed = snapshot.book.levels(isin, bid);
            require(std::equal(expected.begin(), expected.end(), actual.begin(), actual.end()),
                    "ORDLOG equals independent truth aggregates");
            require(actual == replayed, "ORDLOG equals recomputed ORDBOOK snapshot at same revision");
        }
    require(log.book.order_count() == truth.size(), "all active individual orders retained");
}

static void allocation_reuse() {
    Harness h(128, 32);
    h.online();
    auto add = row(h.schema[2], 1, 11, 100000, 4);
    auto remove = row(h.schema[2], 1, 11, 100000, 0, 0);
    allocations = 0;
    count_allocations = true;
    for (int i = 0; i < 10000; ++i) {
        h.begin();
        h.apply(add);
        h.commit();
        h.begin();
        h.apply(remove);
        h.commit();
    }
    count_allocations = false;
    require(allocations == 0, "no heap allocations on steady callback/decode/book/commit path");
    require(h.book.memory_bytes() < 200000, "reported memory stays bounded");
}
static void review_semantics() {
    Harness h(4, 16);
    h.online();
    h.begin();
    h.commit();
    require(h.book.valid() && h.book.revision(11) == 1, "empty ONLINE commit has a publishable revision");
    h.begin();
    h.add(1, 11, 100000, 10, 1, 1);
    h.commit();
    h.begin();
    h.add(1, 11, 100000, 6, 2, 2, 0x4);
    h.commit();
    require(h.book.levels(11, true).at(100000) == 6, "NonQuote fill still updates an admitted order");
    h.begin();
    h.add(1, 11, 100000, 6, 0, 3, 0x8000000);
    h.commit();
    require(h.book.levels(11, true).empty(), "flagged cancel removes an admitted outright order");
    h.begin();
    for (std::int64_t id = 10; id < 16; ++id)
        h.add(id, 11, 100000, 10, 1, id, 0x4);
    h.add(15, 11, 100000, 6, 2, 20, 0);
    h.add(99, 11, 100000, 2, 2, 21, 0);
    h.commit();
    const auto counters = h.book.metrics();
    require(h.book.valid() && counters.excluded_ids == 4 && counters.excluded_evictions == 2 &&
                counters.ignored_executions == 2 && counters.excluded_executions == 1,
            "excluded IDs are bounded and unknown executions are counted without recovery");
    h.begin();
    h.apply(row(h.schema[0], 2, 11, 200000, 10, 1, 30));
    h.commit();
    h.begin();
    h.add(2, 11, 200000, 8, 2, 40);
    h.add(2, 11, 200000, 6, 2, 50);
    h.commit();
    h.begin();
    auto stale = row(h.schema[2], 2, 11, 0, 0, 0, 70);
    stale.set("replID", std::int64_t{40});
    stale.set("replAct", std::int64_t{1});
    h.apply(stale);
    h.commit();
    require(h.book.order_count() == 1 && h.book.levels(11, true).at(200000) == 6,
            "replAct of an old operation cannot erase the current order");
    h.begin();
    h.add(2, 11, 200000, 4, 2, 45);
    h.commit();
    require(h.book.levels(11, true).at(200000) == 6, "an older source operation cannot replace the latest row");
    h.clear(0, 31);
    h.begin();
    h.commit();
    require(h.book.order_count() == 1 && h.book.levels(11, true).at(200000) == 6,
            "snapshot clear cannot retire an order whose current source is the log");
    const auto cleared = h.book.metrics().clear_deleted_erased;
    h.clear(2, 51);
    h.begin();
    h.commit();
    require(h.book.order_count() == 0 && h.book.metrics().clear_deleted_erased == cleared + 1,
            "partial log clear retains its per-source revision behavior and reports erasure");
    h.begin();
    h.apply(row(h.schema[0], 3, 11, 300000, 5, 1, 80));
    h.commit();
    h.begin();
    h.clear(2, std::numeric_limits<std::int64_t>::max());
    h.commit();
    require(!h.book.valid() && h.book.order_count() == 0, "log MAX also clears snapshot-only orders");
    h.online();
    h.begin();
    h.commit();
    require(!h.book.valid() && h.book.needs_fresh_snapshot(),
            "MAX cannot reacquire readiness without a fresh snapshot");
    h.begin();
    h.apply(row(h.schema[0], 4, 11, 400000, 3));
    h.commit();
    h.online();
    require(h.book.needs_fresh_snapshot(), "ONLINE cannot disarm the snapshot wait before commit");
    h.begin();
    h.commit();
    require(h.book.valid() && !h.book.needs_fresh_snapshot() && h.book.order_count() == 1,
            "fresh snapshot and committed ONLINE recover MAX");
}
static void life_time_and_cross() {
    Harness h;
    require(!h.book.on_plaza2_listener_event({.kind = Kind::LifeNum, .unsigned_value = 5}), "snapshot life");
    h.begin();
    h.apply(row(h.schema[0], 1, 11, 100000, 10));
    Row info(h.schema[1]);
    info.set("publication_state", std::int8_t{1});
    info.set("trades_lifenum", std::int64_t{8});
    h.apply(info);
    h.commit();
    const auto epoch = h.book.epoch();
    require(!h.book.on_plaza2_listener_event({.kind = Kind::LifeNum, .unsigned_value = 8}), "online log life");
    h.online();
    h.begin();
    h.commit();
    require(h.book.valid() && h.book.epoch() == epoch && h.book.order_count() == 1,
            "ORDBOOK life then info-bound ORDLOG life preserves snapshot");
    require(!h.book.on_plaza2_listener_event({.kind = Kind::LifeNum, .unsigned_value = 5}), "unchanged snapshot life");
    require(h.book.valid() && h.book.committed_log_lifenum() == 8,
            "a repeated snapshot life does not masquerade as a log-life change");
    const auto utc =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    h.begin();
    auto trade = row(h.schema[2], 1, 11, 100000, 6, 2, 2);
    trade.set("moment_ns", static_cast<std::uint64_t>(utc - 2000000));
    h.apply(trade);
    h.commit();
    require(h.book.exchange_moment_ns(11) == static_cast<std::uint64_t>(utc - 2000000),
            "exact source ns reaches committed book");
    require(h.book.metrics().lag_samples == 1 && h.book.metrics().lag_last_ns >= 2000000,
            "UTC exchange-to-commit lag is aggregated");
    std::size_t transitions{};
    h.book.on_crossed = [&](auto isin, bool) {
        require(isin == 11, "crossed scope");
        ++transitions;
    };
    h.begin();
    h.add(2, 11, 90000, 2, 1, 3, 1, 2);
    h.commit();
    require(h.book.crossed(11) && !h.book.crossed(22) && transitions == 1, "crossed committed instrument is flagged");
    h.begin();
    h.add(1, 11, 100000, 0, 0, 4);
    h.commit();
    require(!h.book.crossed(11) && transitions == 2, "uncross transition is counted once");
    require(!h.book.on_plaza2_listener_event({.kind = Kind::LifeNum, .unsigned_value = 9}), "changed online life");
    require(!h.book.valid() && h.book.order_count() == 0, "actual life change clears the generation");
    Harness changed_info;
    changed_info.begin();
    Row first(changed_info.schema[1]);
    first.set("publication_state", std::int8_t{1});
    first.set("trades_lifenum", std::int64_t{8});
    changed_info.apply(first);
    changed_info.commit();
    changed_info.online();
    changed_info.begin();
    changed_info.commit();
    changed_info.begin();
    first.set("trades_lifenum", std::int64_t{9});
    changed_info.apply(first);
    require(static_cast<bool>(changed_info.book.on_plaza2_listener_event({.kind = Kind::TransactionCommit})) &&
                !changed_info.book.valid(),
            "changed authoritative info log life requests a fresh bootstrap");
}
static void publication_lag() {
    const auto utc_now_ns = [] {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
    };
    Harness h;
    h.online();
    h.begin();
    h.add(1, 11, 100000, 2);
    h.commit();
    std::size_t callbacks{};
    std::uint64_t publication_finished_ns{};
    h.book.on_crossed = [&](auto isin, bool crossed) {
        require(isin == 11 && crossed, "lag fixture crosses only its target instrument");
        ++callbacks;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        publication_finished_ns = utc_now_ns();
    };
    h.begin();
    auto ask = row(h.schema[2], 2, 11, 90000, 3, 1, 2, 1, 2);
    const auto exchange_ns = utc_now_ns();
    ask.set("moment_ns", exchange_ns);
    h.apply(ask);
    h.commit();
    const auto metrics = h.book.metrics();
    require(callbacks == 1 && h.book.crossed(11) && metrics.lag_samples == 1,
            "delayed publication contributes one committed lag sample");
    require(publication_finished_ns >= exchange_ns + 10000000 &&
                metrics.lag_last_ns >= publication_finished_ns - exchange_ns,
            "exchange-to-commit lag includes publication and its delayed crossed callback");
}
static void unframed_snapshot_diagnostics() {
    using namespace full_order_log_test;
    using Kind = Plaza2ListenerEventKind;
    const auto info = [](Harness& h, std::int8_t publication = 1, std::int64_t revision = 100) {
        Row row(h.schema[1]);
        row.set("publication_state", publication);
        row.set("trades_rev", revision);
        row.set("trades_lifenum", std::int64_t{7});
        return row;
    };
    Harness observed;
    std::size_t publications{};
    observed.book.on_commit = [&](const auto&) { ++publications; };
    observed.clear(0, INT64_MAX);
    observed.clear(1, INT64_MAX);
    // Exact captured order: Open, two MAX clears,36 filtered128-byte rows, tracked row at40.
    for (unsigned n = 1; n <= 36; ++n)
        observed.apply(row(observed.schema[0], n, 33, 100000, 2));
    observed.apply(row(observed.schema[0], 101, 11, 100000, 2));
    require(!observed.book.valid() && observed.book.order_count() == 0 && observed.book.levels(11, true).empty() &&
                publications == 0,
            "unframed snapshot stays entirely hidden");
    observed.apply(info(observed));
    observed.online();
    require(observed.book.valid() && observed.book.order_count() == 1 && publications == 1 &&
                observed.book.levels(11, true).at(100000) == 2 && observed.book.committed_log_lifenum() == 7,
            "complete unframed snapshot publishes exactly once at ONLINE");
    auto error = observed.book.on_plaza2_listener_event(row(observed.schema[2], 101, 11, 100000, 1, 2).event());
    require(error.code == Plaza2ErrorCode::DecodeFailed &&
                error.message.find("row outside transaction table=orders_log online_seen=yes") != std::string::npos,
            "post-ONLINE unframed business row remains a validation failure");
    require(!observed.book.valid() && observed.book.order_count() == 0, "post-ONLINE error invalidates the book");

    Harness mixed;
    mixed.apply(row(mixed.schema[0], 1, 11, 100000, 2));
    mixed.begin();
    mixed.apply(row(mixed.schema[0], 2, 11, 100000, 3));
    mixed.apply(info(mixed));
    mixed.commit();
    require(!mixed.book.valid() && mixed.book.levels(11, true).empty(), "snapshot TN_COMMIT cannot publish");
    mixed.apply(row(mixed.schema[2], 1, 11, 100000, 1, 2, 2));
    mixed.apply(Row(mixed.schema[3]));
    mixed.online();
    require(mixed.book.valid() && mixed.book.levels(11, true).at(100000) == 4,
            "mixed framed/unframed orders, info, log and system rows share one hidden snapshot");
    mixed.begin();
    mixed.add(1, 11, 100000, 0, 2, 3);
    require(mixed.book.levels(11, true).at(100000) == 4, "online updates stay hidden until commit");
    mixed.commit();
    require(mixed.book.levels(11, true).at(100000) == 3, "framed online updates remain commit-atomic");
    mixed.clear(1, INT64_MAX);
    require(!mixed.book.valid() && mixed.book.order_count() == 0, "MAX info clear retires snapshot authority");

    Harness online_in_frame;
    unsigned online_publications{};
    online_in_frame.book.on_commit = [&](const auto&) { ++online_publications; };
    online_in_frame.apply(row(online_in_frame.schema[0], 1, 11, 100000, 2));
    online_in_frame.begin();
    online_in_frame.apply(info(online_in_frame));
    online_in_frame.online();
    require(online_in_frame.book.valid() && online_publications == 1,
            "ONLINE commits a mixed snapshot inside an open frame");
    online_in_frame.commit();
    require(online_publications == 1, "trailing snapshot TN_COMMIT does not publish the same snapshot twice");
    Harness interrupted;
    interrupted.apply(row(interrupted.schema[0], 1, 11, 100000, 2));
    interrupted.apply(info(interrupted));
    interrupted.control(Kind::Close);
    require(!interrupted.book.valid() && interrupted.book.order_count() == 0 &&
                interrupted.book.levels(11, true).empty(),
            "Close mid-snapshot discards every hidden row");
    Harness empty;
    empty.clear(0, INT64_MAX);
    empty.clear(1, INT64_MAX);
    empty.apply(info(empty));
    empty.online();
    require(empty.book.valid() && empty.book.order_count() == 0 && empty.book.revision(11) > 0,
            "publication-complete empty snapshot becomes visible at ONLINE");
    Harness incomplete;
    incomplete.apply(row(incomplete.schema[0], 1, 11, 100000, 2));
    incomplete.apply(info(incomplete, 0));
    incomplete.online();
    incomplete.begin();
    incomplete.commit();
    require(incomplete.book.valid() && incomplete.book.levels(11, true).at(100000) == 2,
            "optional publication0 callback does not veto CGate's authoritative ONLINE");
    Harness internal_info;
    std::size_t native_publications{};
    internal_info.book.on_commit = [&](const auto&) { ++native_publications; };
    internal_info.clear(0, INT64_MAX);
    internal_info.clear(1, INT64_MAX);
    internal_info.apply(row(internal_info.schema[0], 1, 11, 100000, 2));
    require(!internal_info.book.valid() && internal_info.book.levels(11, true).empty(), "no early publication");
    internal_info.online();
    require(internal_info.book.valid() && native_publications == 1 &&
                !internal_info.book.committed_log_lifenum().has_value(),
            "ONLINE completes a no-info snapshot without inventing a log life");
    internal_info.online();
    require(native_publications == 1, "duplicate ONLINE does not republish");
    Harness framed_no_info;
    framed_no_info.begin();
    framed_no_info.apply(row(framed_no_info.schema[0], 1, 11, 100000, 2));
    framed_no_info.commit();
    framed_no_info.online();
    require(!framed_no_info.book.valid(), "fully framed no-info snapshot retains next-commit visibility");
    framed_no_info.begin();
    framed_no_info.commit();
    require(framed_no_info.book.valid(), "fully framed snapshot does not require an info callback");
    Harness native_empty;
    native_empty.clear(0, INT64_MAX);
    native_empty.clear(1, INT64_MAX);
    native_empty.online();
    require(native_empty.book.valid() && native_empty.book.order_count() == 0,
            "empty no-info snapshot completes at ONLINE");
    Harness short_row;
    auto payload = row(short_row.schema[0], 1, 11, 100000, 2);
    auto event = payload.event();
    event.raw_payload = event.raw_payload.first(127);
    error = short_row.book.on_plaza2_listener_event(event);
    require(error.message.find("row size table=orders actual=127 expected=128") != std::string::npos,
            "snapshot size validation runs independently of absent TN_BEGIN");
    short_row.open();
    error = short_row.book.on_plaza2_listener_event(info(short_row, 1, -1).event());
    require(error.message.find("invalid snapshot info") != std::string::npos,
            "negative trades_rev cannot complete an unframed snapshot");
    error = short_row.book.on_plaza2_listener_event({.kind = Kind::Online});
    require(error && !short_row.book.valid(), "ONLINE after a rejected row cannot bypass a fresh bound OPEN");
}
int main() {
    try {
        unframed_snapshot_diagnostics();
        semantics();
        committed_source_session();
        session_rollover_deletions();
        recovery();
        independent_snapshot();
        allocation_reuse();
        review_semantics();
        life_time_and_cross();
        publication_lag();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "Full Order Log semantic, snapshot, recovery and allocation tests passed\n";
}
