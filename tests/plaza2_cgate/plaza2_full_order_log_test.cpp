#include "full_order_log_test_support.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <random>

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
    const auto other_revision = h.book.revision(22);
    h.begin();
    h.add(1, 11, 1234567, 6, 2, 2);
    require(h.book.levels(11, true).at(1234567) == 10, "uncommitted remainder is invisible");
    h.commit();
    require(h.book.levels(11, true).at(1234567) == 6, "partial trade replaces direct remainder");
    require(h.book.changes().size() == 1 && h.book.changes()[0].quantity == 6, "one incremental level change");
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
    Row multileg(h.schema[3]);
    h.apply(multileg);
    h.add(7, 11, 100000, 10, 1, 10);
    h.add(7, 11, 100000, 4, 2, 11, 0x2);
    h.commit();
    require(h.book.order_count() == 1 && h.book.levels(11, true).at(100000) == 4,
            "IOC remainders, NonQuote and multileg excluded; resting IOC-flagged execution applied");
    h.begin();
    const auto before = h.book.metrics();
    auto excluded = row(h.schema[2], 9, 33, 0, 1);
    excluded.nulls[excluded.field("price").index] = 1;
    h.apply(excluded);
    h.commit();
    require(h.book.order_count() == 1 && h.book.metrics().rows_total == before.rows_total + 1 &&
                h.book.metrics().rows_filtered == before.rows_filtered + 1,
            "ISIN filter precedes other field validation and filtered rows enter total rate");
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
    require(h.book.order_count() == 0, "snapshot-source revision remains available after online updates");
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
    require(!h.book.valid(), "MAX cannot reacquire readiness without a fresh snapshot");
    h.begin();
    h.apply(row(h.schema[0], 4, 11, 400000, 3));
    h.commit();
    h.online();
    h.begin();
    h.commit();
    require(h.book.valid() && h.book.order_count() == 1, "fresh snapshot and ONLINE recover MAX");
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
    require(h.book.valid() && h.book.life_state().info_trades_lifenum == 8,
            "a repeated snapshot life does not masquerade as a log-life change");
    const auto utc =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    h.begin();
    auto trade = row(h.schema[2], 1, 11, 100000, 6, 2, 2);
    trade.set("moment_ns", static_cast<std::uint64_t>(utc - 2000000));
    h.apply(trade);
    h.commit();
    require(h.book.exchange_moment_ns(11) == static_cast<std::uint64_t>(utc - 2000000) &&
                h.book.changes()[0].exchange_moment_ns == h.book.exchange_moment_ns(11),
            "exact source ns reaches committed changes");
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
int main() {
    try {
        semantics();
        recovery();
        independent_snapshot();
        allocation_reuse();
        review_semantics();
        life_time_and_cross();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "Full Order Log semantic, snapshot, recovery and allocation tests passed\n";
}
