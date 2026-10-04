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
    auto excluded = row(h.schema[2], 9, 33, 0, 1);
    excluded.nulls[excluded.field("price").index] = 1;
    h.apply(excluded);
    h.commit();
    require(h.book.order_count() == 1, "ISIN filter precedes other field validation");
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
    require(h.book.order_count() == 1 && h.book.levels(22, true).empty(), "ClearDeleted MAX erases affected table");
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
int main() {
    try {
        semantics();
        recovery();
        independent_snapshot();
        allocation_reuse();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "Full Order Log semantic, snapshot, recovery and allocation tests passed\n";
}
