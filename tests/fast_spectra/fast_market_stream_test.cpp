#include "fast_test_encoder.hpp"
#include "moex/fast/fast_book.hpp"
#include "moex/fast/fast_market_stream.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace moex::fast;
using fast_test::Bytes;
using fast_test::Dec;
using fast_test::IncrementalEntry;
using fast_test::SnapshotEntry;

namespace {

#define REQUIRE(condition)                                                                                             \
    do {                                                                                                               \
        if (!(condition))                                                                                              \
            throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition);       \
    } while (false)

constexpr std::size_t kDepth = 5;
using Book = AggregateBook<kDepth>;

struct BookSink : StreamSink {
    std::vector<Book> books;
    std::vector<std::uint64_t> ids;
    int all_stale{}, resets{}, batches{};

    void on_instrument(std::uint32_t slot, std::uint64_t id) {
        REQUIRE(slot == books.size());
        books.emplace_back();
        ids.push_back(id);
    }
    void on_book_clear(std::uint32_t slot) {
        books[slot].clear();
    }
    void on_snapshot(std::uint32_t slot, const Snapshot&, const Snapshot::MDEntries& e) {
        books[slot].apply(e);
    }
    void on_incremental(std::uint32_t slot, const Incremental&, const Incremental::MDEntries& e) {
        books[slot].apply(e);
    }
    void on_all_stale() {
        ++all_stale;
    }
    void on_sequence_reset() {
        ++resets;
        for (Book& book : books)
            book.clear();
    }
    void on_batch_end() {
        ++batches;
    }
    [[nodiscard]] std::uint32_t slot_of(std::uint64_t id) const {
        const auto found = std::find(ids.begin(), ids.end(), id);
        REQUIRE(found != ids.end());
        return static_cast<std::uint32_t>(found - ids.begin());
    }
};

using Stream = MarketStream<BookSink>;

IncrementalEntry bid(std::uint32_t action, std::uint64_t id, std::uint32_t rpt, std::uint64_t level,
                     std::int64_t price, std::int64_t size) {
    return {action, '0', id, rpt, level, Dec{price, 0}, size};
}

void feed(Stream& stream, const Bytes& packet, std::uint64_t now = 0) {
    stream.on_incremental(packet.data(), packet.size(), now);
}
void snap(Stream& stream, const Bytes& packet) {
    stream.on_snapshot(packet.data(), packet.size());
}
std::int64_t best_bid(const BookSink& sink, std::uint64_t id) {
    const auto* level = sink.books[sink.slot_of(id)].bids().best();
    return level ? level->price.mantissa : -1;
}

// Joins mid-day: incrementals are queued, a snapshot restores the book and the
// queue is replayed past the snapshot's LastMsgSeqNumProcessed.
void late_join_recovers_from_snapshot() {
    BookSink sink;
    Stream stream(sink);
    REQUIRE(!stream.needs_snapshot());
    feed(stream, fast_test::incremental(100, {bid(1, 7, 41, 1, 500, 10)}));
    feed(stream, fast_test::incremental(101, {bid(1, 7, 42, 1, 501, 11)}));
    feed(stream, fast_test::incremental(102, {bid(1, 7, 43, 1, 502, 12)}));
    REQUIRE(stream.needs_snapshot() && stream.unsynced() == 1 && !stream.synced(0));
    REQUIRE(sink.books[0].bids().count == 0 && stream.stats().queued == 3);

    // Snapshot taken after incremental 101 (RptSeq 42); we joined the cycle midway.
    snap(stream, fast_test::snapshot(5, 7, 42, 101, {{'0', 1, Dec{501, 0}, 11}, {'1', 1, Dec{510, 0}, 3}}));
    REQUIRE(!stream.synced(0));
    snap(stream, fast_test::sequence_reset(6, 1));
    REQUIRE(stream.synced(0) && stream.unsynced() == 0 && best_bid(sink, 7) == 502);
    REQUIRE(sink.books[0].asks().best()->price.mantissa == 510 && stream.stats().snapshots_applied == 1);

    feed(stream, fast_test::incremental(103, {bid(1, 7, 44, 1, 503, 13)}));
    REQUIRE(best_bid(sink, 7) == 503 && stream.stats().queued == 3);

    // The first cycle was joined midway, so absent instruments are not yet known
    // to be empty; a complete cycle settles that and releases the snapshot feed.
    REQUIRE(stream.needs_snapshot());
    snap(stream, fast_test::snapshot(1, 7, 44, 103, {{'0', 1, Dec{503, 0}, 13}}));
    snap(stream, fast_test::sequence_reset(2, 1));
    REQUIRE(!stream.needs_snapshot() && best_bid(sink, 7) == 503);

    // A brand-new instrument now starts from an empty book without a snapshot.
    feed(stream, fast_test::incremental(104, {bid(0, 8, 1, 1, 900, 1)}));
    REQUIRE(stream.synced(sink.slot_of(8)) && best_bid(sink, 8) == 900 && !stream.needs_snapshot());
}

Stream& synced_stream(BookSink& sink, Stream& stream) {
    feed(stream, fast_test::incremental(10, {bid(0, 1, 5, 1, 100, 1), bid(0, 2, 8, 1, 200, 1)}));
    snap(stream, fast_test::sequence_reset(50, 1));
    snap(stream, fast_test::snapshot(1, 1, 5, 10, {{'0', 1, Dec{100, 0}, 1}}));
    snap(stream, fast_test::snapshot(2, 2, 8, 10, {{'0', 1, Dec{200, 0}, 1}}));
    snap(stream, fast_test::sequence_reset(3, 1));
    REQUIRE(stream.unsynced() == 0 && !stream.needs_snapshot() && best_bid(sink, 1) == 100);
    return stream;
}

void arbitrates_and_reorders() {
    BookSink sink;
    Stream stream(sink);
    synced_stream(sink, stream);
    const Bytes p11 = fast_test::incremental(11, {bid(1, 1, 6, 1, 101, 1)});
    const Bytes p12 = fast_test::incremental(12, {bid(1, 1, 7, 1, 102, 1)});
    const Bytes p13 = fast_test::incremental(13, {bid(1, 1, 8, 1, 103, 1)});
    feed(stream, p11);
    feed(stream, p11); // feed B copy
    feed(stream, p13, 1000);
    REQUIRE(best_bid(sink, 1) == 101 && stream.next_sequence() == 12);
    feed(stream, p13, 1100);
    feed(stream, p12, 1200);
    REQUIRE(best_bid(sink, 1) == 103 && stream.next_sequence() == 14);
    feed(stream, p12, 1300);
    REQUIRE(stream.stats().gaps == 0 && stream.stats().duplicates == 3 && stream.stats().reordered == 1);
    REQUIRE(sink.all_stale == 0 && stream.unsynced() == 0);
}

void loss_triggers_snapshot_recovery() {
    BookSink sink;
    StreamConfig config;
    config.reorder_timeout_ns = 1000;
    Stream stream(sink, config);
    synced_stream(sink, stream);
    // 11 is lost on both feeds.
    feed(stream, fast_test::incremental(12, {bid(1, 1, 7, 1, 102, 1)}), 5000);
    REQUIRE(stream.unsynced() == 0);
    stream.on_timer(5500);
    REQUIRE(stream.unsynced() == 0);
    stream.on_timer(6000);
    REQUIRE(sink.all_stale == 1 && stream.unsynced() == 2 && stream.needs_snapshot() && stream.stats().gaps == 1);
    REQUIRE(stream.next_sequence() == 13 && best_bid(sink, 1) == 100);
    feed(stream, fast_test::incremental(13, {bid(1, 1, 8, 1, 103, 1)}), 7000);

    // A snapshot that predates the loss is refused: it cannot bridge packet 11.
    snap(stream, fast_test::snapshot(1, 1, 5, 10, {{'0', 1, Dec{100, 0}, 1}}));
    snap(stream, fast_test::sequence_reset(2, 1));
    REQUIRE(stream.unsynced() == 2 && stream.stats().snapshots_too_old == 1);

    // A complete cycle generated at 12: instrument 1 is restored, instrument 2 is
    // absent from the cycle and therefore empty.
    snap(stream, fast_test::snapshot(1, 1, 7, 12, {{'0', 1, Dec{102, 0}, 1}}));
    snap(stream, fast_test::sequence_reset(2, 1));
    REQUIRE(stream.unsynced() == 0 && best_bid(sink, 1) == 103 && best_bid(sink, 2) == -1);
    REQUIRE(stream.stats().empty_adopted == 1 && !stream.needs_snapshot());
    feed(stream, fast_test::incremental(14, {bid(0, 2, 30, 1, 250, 1), bid(1, 1, 9, 1, 104, 1)}), 8000);
    REQUIRE(best_bid(sink, 2) == 250 && best_bid(sink, 1) == 104 && stream.unsynced() == 0);
}

void far_jump_and_instrument_gap() {
    BookSink sink;
    Stream stream(sink);
    synced_stream(sink, stream);
    // RptSeq jumps for instrument 1 only: it alone goes stale.
    feed(stream, fast_test::incremental(11, {bid(1, 1, 9, 1, 109, 1), bid(1, 2, 9, 1, 209, 1)}));
    REQUIRE(stream.unsynced() == 1 && !stream.synced(sink.slot_of(1)) && stream.synced(sink.slot_of(2)));
    REQUIRE(best_bid(sink, 2) == 209 && sink.all_stale == 0 && stream.stats().instrument_gaps == 1);
    snap(stream, fast_test::snapshot(7, 1, 9, 11, {{'0', 1, Dec{109, 0}, 1}}));
    snap(stream, fast_test::sequence_reset(8, 1));
    REQUIRE(stream.unsynced() == 0 && best_bid(sink, 1) == 109 && best_bid(sink, 2) == 209);

    // A jump beyond the reorder window is a loss without waiting for the timer.
    feed(stream, fast_test::incremental(5000, {bid(1, 2, 10, 1, 210, 1)}));
    REQUIRE(sink.all_stale == 1 && stream.unsynced() == 2 && stream.next_sequence() == 5001);
}

void fragmented_snapshots() {
    BookSink sink;
    Stream stream(sink);
    feed(stream, fast_test::incremental(20, {bid(1, 1, 3, 1, 100, 1), bid(1, 2, 3, 1, 200, 1)}));
    snap(stream, fast_test::sequence_reset(90, 1));
    // Instrument 1 arrives in two fragments.
    snap(stream, fast_test::snapshot(1, 1, 3, 20, {{'0', 1, Dec{100, 0}, 1}}, 0));
    snap(stream, fast_test::snapshot(2, 1, 3, 20, {{'0', 2, Dec{99, 0}, 2}}, 1));
    // Instrument 2 loses its first fragment (3); seeing only the tail must not restore it.
    for (int copy = 0; copy < 8; ++copy)
        snap(stream, fast_test::snapshot(4, 2, 3, 20, {{'0', 2, Dec{199, 0}, 2}}, 1));
    snap(stream, fast_test::sequence_reset(5, 1));
    REQUIRE(stream.synced(sink.slot_of(1)) && sink.books[sink.slot_of(1)].bids().count == 2);
    REQUIRE(!stream.synced(sink.slot_of(2)) && stream.stats().snapshot_resyncs >= 1);
    // The next cycle delivers it whole.
    snap(stream, fast_test::snapshot(1, 2, 3, 20, {{'0', 1, Dec{200, 0}, 1}}, 0));
    snap(stream, fast_test::snapshot(2, 2, 3, 20, {{'0', 2, Dec{199, 0}, 2}}, 1));
    snap(stream, fast_test::sequence_reset(3, 1));
    REQUIRE(stream.unsynced() == 0 && sink.books[sink.slot_of(2)].bids().count == 2 && best_bid(sink, 2) == 200);
}

void sequence_reset_and_heartbeats() {
    BookSink sink;
    Stream stream(sink);
    synced_stream(sink, stream);
    // Heartbeat that does not consume a number: the next data packet reuses it.
    feed(stream, fast_test::heartbeat(11));
    feed(stream, fast_test::incremental(11, {bid(1, 1, 6, 1, 101, 1)}));
    REQUIRE(best_bid(sink, 1) == 101 && stream.next_sequence() == 12);
    // Heartbeat that does consume one: the next data packet follows it.
    feed(stream, fast_test::heartbeat(12));
    feed(stream, fast_test::incremental(13, {bid(1, 1, 7, 1, 102, 1)}));
    REQUIRE(best_bid(sink, 1) == 102 && stream.next_sequence() == 14 && stream.stats().gaps == 0);
    REQUIRE(sink.all_stale == 0 && stream.unsynced() == 0);

    feed(stream, fast_test::sequence_reset(14, 1));
    REQUIRE(sink.resets == 1 && stream.next_sequence() == 1 && best_bid(sink, 1) == -1 && stream.unsynced() == 0);
    feed(stream, fast_test::incremental(1, {bid(0, 1, 1, 1, 300, 1), bid(0, 3, 1, 1, 400, 1)}));
    REQUIRE(best_bid(sink, 1) == 300 && best_bid(sink, 3) == 400 && !stream.needs_snapshot());

    // Garbage and runts are counted, not trusted.
    const Bytes runt{1, 0, 0};
    feed(stream, runt);
    Bytes corrupt = fast_test::incremental(2, {bid(1, 1, 2, 1, 301, 1)});
    corrupt.resize(corrupt.size() - 3);
    feed(stream, corrupt);
    REQUIRE(stream.stats().malformed == 2 && sink.all_stale == 1 && stream.needs_snapshot());
}

// An Empty Book entry is a complete statement of the book, so it ends the wait for a snapshot.
void empty_book_resumes_without_snapshot() {
    BookSink sink;
    Stream stream(sink);
    feed(stream, fast_test::incremental(30, {bid(1, 1, 7, 1, 100, 1), bid(1, 2, 4, 1, 200, 1)}));
    REQUIRE(stream.unsynced() == 2);
    IncrementalEntry empty;
    empty.type = 'J';
    empty.security_id = 1;
    empty.rpt_seq = 8;
    feed(stream, fast_test::incremental(31, {empty, bid(0, 1, 9, 1, 150, 2), bid(1, 2, 5, 1, 201, 1)}));
    REQUIRE(stream.synced(sink.slot_of(1)) && !stream.synced(sink.slot_of(2)) && stream.unsynced() == 1);
    REQUIRE(best_bid(sink, 1) == 150 && sink.books[sink.slot_of(1)].bids().count == 1);
    // A snapshot older than the Empty Book must not be replayed over it, and the stale queue entry is dropped.
    snap(stream, fast_test::sequence_reset(9, 1));
    snap(stream, fast_test::snapshot(1, 2, 5, 31, {{'0', 1, Dec{201, 0}, 1}}));
    snap(stream, fast_test::sequence_reset(2, 1));
    REQUIRE(stream.unsynced() == 0 && best_bid(sink, 1) == 150 && best_bid(sink, 2) == 201);
    feed(stream, fast_test::incremental(32, {bid(1, 1, 10, 1, 151, 2)}));
    REQUIRE(best_bid(sink, 1) == 151 && stream.stats().instrument_gaps == 0);
}

void recovery_queue_overflow() {
    BookSink sink;
    StreamConfig config;
    config.recovery_queue_bytes = 256;
    Stream stream(sink, config);
    for (std::uint32_t seq = 1; seq <= 40; ++seq)
        feed(stream, fast_test::incremental(seq, {bid(1, 1, seq, 1, 100 + seq, 1)}));
    REQUIRE(stream.stats().queue_dropped > 0 && stream.unsynced() == 1);
    // Whatever survived the overflow still leads to a consistent book.
    snap(stream, fast_test::snapshot(1, 1, 40, 40, {{'0', 1, Dec{140, 0}, 1}}));
    snap(stream, fast_test::sequence_reset(2, 1));
    REQUIRE(stream.unsynced() == 0 && best_bid(sink, 1) == 140);
    feed(stream, fast_test::incremental(41, {bid(1, 1, 41, 1, 141, 1)}));
    REQUIRE(best_bid(sink, 1) == 141);
}

// Reference model: random book activity pushed through a lossy, duplicating,
// reordering A/B channel with snapshot cycles shaped like production's: a cycle
// is generated at one instant (every message carries that LastMsgSeqNumProcessed)
// and then trickles out while incrementals keep flowing. Whenever the stream
// calls an instrument synced, its book must equal the truth as of the last
// packet the stream has consumed.
struct Truth {
    struct Side {
        std::vector<std::pair<std::int64_t, std::int64_t>> levels;
    };
    struct Instrument {
        Side bids, asks;
        std::uint32_t rpt_seq{};
    };
    std::vector<Instrument> instruments;
};

bool equal(const Book::Side& actual, const Truth::Side& expected) {
    if (actual.count != expected.levels.size())
        return false;
    for (std::uint32_t i = 0; i < actual.count; ++i)
        if (actual.levels[i].price.mantissa != expected.levels[i].first ||
            actual.levels[i].size != expected.levels[i].second)
            return false;
    return true;
}

void randomized_lossy_channel(std::uint64_t seed) {
    std::uint64_t state = seed * 0x9E3779B97F4A7C15ull + 1;
    const auto next = [&](std::uint64_t bound) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state % bound;
    };
    constexpr std::size_t kInstruments = 6;
    constexpr std::uint32_t kPackets = 6000;
    constexpr std::uint32_t kQuietFrom = 5000; // loss stops here so the stream can settle
    Truth truth;
    truth.instruments.resize(kInstruments);
    std::map<std::uint32_t, Truth> history; // truth after each incremental seq
    BookSink sink;
    StreamConfig config;
    config.reorder_window = 16;
    config.reorder_timeout_ns = 800;
    Stream stream(sink, config);
    std::uint64_t now = 0;
    std::uint32_t snapshot_seq = 1;
    std::deque<Bytes> delayed;
    std::deque<Bytes> cycle; // snapshot messages generated but not yet sent

    const auto verify = [&] {
        for (std::uint32_t slot = 0; slot < sink.ids.size(); ++slot) {
            if (!stream.synced(slot))
                continue;
            const std::uint32_t consumed = stream.applied_through(slot);
            const auto at = history.find(consumed);
            if (at == history.end())
                continue;
            const Truth::Instrument& expected = at->second.instruments[sink.ids[slot] - 1];
            if (!equal(sink.books[slot].bids(), expected.bids) || !equal(sink.books[slot].asks(), expected.asks))
                throw std::runtime_error("book mismatch: seed " + std::to_string(seed) + " seq " +
                                         std::to_string(consumed) + " security " + std::to_string(sink.ids[slot]));
        }
    };
    const auto send_incremental = [&](const Bytes& packet, bool lossy) {
        for (int copy = 0; copy < 2; ++copy) { // feeds A and B
            if (lossy && next(100) < 12)
                continue;
            if (lossy && next(100) < 10) {
                delayed.push_back(packet);
                continue;
            }
            feed(stream, packet, now);
            verify();
            while (!delayed.empty() && next(3) == 0) {
                feed(stream, delayed.front(), now);
                delayed.pop_front();
                verify();
            }
        }
    };
    const auto send_snapshot = [&](const Bytes& packet, bool lossy) {
        for (int copy = 0; copy < 2; ++copy)
            if (!lossy || next(100) >= 10)
                snap(stream, packet);
    };

    for (std::uint32_t seq = 1; seq <= kPackets; ++seq) {
        const bool lossy = seq < kQuietFrom;
        now += 100;
        std::vector<IncrementalEntry> entries;
        const std::size_t count = 1 + next(4);
        for (std::size_t n = 0; n < count; ++n) {
            // Instrument 6 stays empty for a long stretch to exercise the absent-means-empty rule.
            const std::size_t index = next(seq < 1500 ? kInstruments - 1 : kInstruments);
            Truth::Instrument& inst = truth.instruments[index];
            const bool is_bid = next(2) == 0;
            Truth::Side& side = is_bid ? inst.bids : inst.asks;
            const std::uint64_t choice = next(10);
            IncrementalEntry entry;
            entry.type = is_bid ? '0' : '1';
            entry.security_id = index + 1;
            entry.rpt_seq = ++inst.rpt_seq;
            const std::int64_t price = 1000 + static_cast<std::int64_t>(next(500));
            const std::int64_t size = 1 + static_cast<std::int64_t>(next(90));
            if (side.levels.empty() || choice < 4) {
                const std::size_t at = next(side.levels.size() + 1);
                if (at >= kDepth) {
                    --inst.rpt_seq;
                    continue;
                }
                side.levels.insert(side.levels.begin() + static_cast<std::ptrdiff_t>(at), {price, size});
                if (side.levels.size() > kDepth)
                    side.levels.pop_back();
                entry.action = 0;
                entry.level = at + 1;
                entry.price = Dec{price, 0};
                entry.size = size;
            } else if (choice < 7) {
                const std::size_t at = next(side.levels.size());
                side.levels[at] = {price, size};
                entry.action = 1;
                entry.level = at + 1;
                entry.price = Dec{price, 0};
                entry.size = size;
            } else {
                const std::size_t at = next(side.levels.size());
                side.levels.erase(side.levels.begin() + static_cast<std::ptrdiff_t>(at));
                entry.action = 2;
                entry.level = at + 1;
            }
            entries.push_back(entry);
        }
        history[seq] = truth;
        if (history.size() > 200)
            history.erase(history.begin());
        send_incremental(fast_test::incremental(seq, entries), lossy);
        stream.on_timer(now);
        verify();

        if (!cycle.empty()) {
            send_snapshot(cycle.front(), lossy);
            cycle.pop_front();
            verify();
            continue;
        }
        if (seq % 8 != 0)
            continue;
        // Generate the next cycle as of this packet; books that are empty are simply not sent.
        for (std::size_t index = 0; index < kInstruments; ++index) {
            const Truth::Instrument& inst = truth.instruments[index];
            std::vector<SnapshotEntry> levels;
            for (std::size_t i = 0; i < inst.bids.levels.size(); ++i)
                levels.push_back({'0', i + 1, Dec{inst.bids.levels[i].first, 0}, inst.bids.levels[i].second});
            for (std::size_t i = 0; i < inst.asks.levels.size(); ++i)
                levels.push_back({'1', i + 1, Dec{inst.asks.levels[i].first, 0}, inst.asks.levels[i].second});
            if (levels.empty())
                continue;
            if (levels.size() > 2 && next(2) == 0) {
                const auto middle = levels.begin() + static_cast<std::ptrdiff_t>(levels.size() / 2);
                cycle.push_back(
                    fast_test::snapshot(snapshot_seq++, index + 1, inst.rpt_seq, seq, {levels.begin(), middle}, 0));
                cycle.push_back(
                    fast_test::snapshot(snapshot_seq++, index + 1, inst.rpt_seq, seq, {middle, levels.end()}, 1));
            } else {
                // Books always flag their snapshots; trades and statistics leave the field out.
                cycle.push_back(fast_test::snapshot(snapshot_seq++, index + 1, inst.rpt_seq, seq, levels,
                                                    next(2) ? std::optional<std::uint64_t>{1} : std::nullopt));
            }
        }
        cycle.push_back(fast_test::sequence_reset(snapshot_seq, 1));
        snapshot_seq = 1;
    }
    REQUIRE(stream.stats().gaps > 0 && stream.stats().snapshots_applied > 0 && stream.stats().replayed > 0);
    REQUIRE(stream.unsynced() == 0 && !stream.needs_snapshot() && stream.next_sequence() == kPackets + 1);
    REQUIRE(sink.ids.size() == kInstruments);
    for (std::uint32_t slot = 0; slot < sink.ids.size(); ++slot) {
        const Truth::Instrument& expected = truth.instruments[sink.ids[slot] - 1];
        REQUIRE(equal(sink.books[slot].bids(), expected.bids) && equal(sink.books[slot].asks(), expected.asks));
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        late_join_recovers_from_snapshot();
        arbitrates_and_reorders();
        loss_triggers_snapshot_recovery();
        far_jump_and_instrument_gap();
        fragmented_snapshots();
        sequence_reset_and_heartbeats();
        empty_book_resumes_without_snapshot();
        recovery_queue_overflow();
        const std::uint64_t seeds = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200;
        for (std::uint64_t seed = 1; seed <= seeds; ++seed)
            randomized_lossy_channel(seed);
    } catch (const std::exception& error) {
        std::cerr << "fast_market_stream_test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
