#include "asts_test_encoder.hpp"
#include "moex/fast/asts/asts_order_book.hpp"
#include "moex/fast/asts/asts_stream.hpp"

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
using namespace moex::fast::asts;
using asts_test::Bytes;
using asts_test::Dec;
using asts_test::Encoder;

namespace {

#define REQUIRE(condition)                                                                                             \
    do {                                                                                                               \
        if (!(condition))                                                                                              \
            throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition);       \
    } while (false)

asts_test::Order add(std::string symbol, std::string id, std::int32_t rpt, char type, std::int64_t price,
                     std::int64_t size) {
    asts_test::Order order;
    order.action = 0;
    order.type = type;
    order.id = std::move(id);
    order.symbol = std::move(symbol);
    order.rpt_seq = rpt;
    order.price = Dec{price, -2};
    order.size = Dec{size, 0};
    order.board = "CETS";
    return order;
}
asts_test::Order remove(std::string symbol, std::string id, std::int32_t rpt, char type) {
    asts_test::Order order = add(std::move(symbol), std::move(id), rpt, type, 0, 0);
    order.action = 2;
    order.size.reset();
    order.null_size = true;
    return order;
}

// The copy operator: a clear presence bit repeats the previous entry's value, an
// explicit NULL ends the repetition, and state never leaks between messages.
void copy_operator_decoding() {
    asts_test::Order first = add("USD000UTSTOM", "101", 7, '0', 9150, 25);
    asts_test::Order second; // repeats action, type, symbol, price, size and board
    second.id = "102";
    second.rpt_seq = 8;
    asts_test::Order third; // new price, size explicitly NULL
    third.id = "103";
    third.rpt_seq = 9;
    third.action = 2;
    third.price = Dec{9175, -2};
    third.null_size = true;
    asts_test::Order fourth; // repeats the NULL size
    fourth.id = "104";
    fourth.rpt_seq = 10;
    const Bytes packet = asts_test::orders(55, {first, second, third, fourth});

    Reader r{packet.data() + kPreambleSize, packet.data() + packet.size()};
    std::uint64_t pmap;
    REQUIRE(read_header(r, pmap) == X_OLR_CURR::kTemplateId);
    X_OLR_CURR message;
    int seen = 0;
    REQUIRE(decode(r, pmap, message, [&](const auto& m, const auto& e) {
        REQUIRE(m.MsgSeqNum == 55 && m.NoMDEntries == 4);
        REQUIRE(e.Symbol.value == "USD000UTSTOM" && e.TradingSessionID.value == "CETS" && e.MDEntryType.value.front() == '0');
        REQUIRE(e.RptSeq.value == 7 + seen && !e.OrderStatus.present && !e.MDEntryTime.present);
        if (seen < 2) {
            REQUIRE(e.MDUpdateAction.value == 0 && e.MDEntryPx.value == (Decimal{9150, -2}) && e.MDEntrySize.value.mantissa == 25);
            REQUIRE(e.MDEntryID.value == (seen ? "102" : "101"));
        } else {
            REQUIRE(e.MDUpdateAction.value == 2 && e.MDEntryPx.value == (Decimal{9175, -2}) && !e.MDEntrySize.present);
        }
        ++seen;
    }));
    REQUIRE(seen == 4 && r.p == r.end);

    // A message whose first entry has no presence bits set sees nothing from the message before it.
    asts_test::Order bare;
    bare.id = "1";
    bare.rpt_seq = 1;
    const Bytes next = asts_test::orders(56, {bare});
    Reader r2{next.data() + kPreambleSize, next.data() + next.size()};
    REQUIRE(read_header(r2, pmap) == X_OLR_CURR::kTemplateId);
    REQUIRE(decode(r2, pmap, message, [&](const auto&, const auto& e) {
        REQUIRE(!e.Symbol.present && !e.MDUpdateAction.present && !e.MDEntryPx.present && e.MDEntryID.value == "1");
    }));

    // Every truncation is rejected rather than read past the buffer.
    for (std::size_t size = kPreambleSize; size < packet.size(); ++size) {
        Reader cut{packet.data() + kPreambleSize, packet.data() + size};
        const bool id_ok = read_header(cut, pmap) == X_OLR_CURR::kTemplateId;
        REQUIRE(!id_ok || !decode(cut, pmap, message, [](const auto&, const auto&) {}));
    }
    REQUIRE(template_name(3710) == "X-OLR-CURR" && template_name(2108) == "0" && kTemplatesSha256.size() == 64);
}

// Arbitrary bytes through every decoder must terminate inside the buffer (the sanitizer build checks the latter).
void random_input() {
    std::uint64_t state = 0x13198A2E03707344ull;
    const auto next = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    const auto ignore = [](const auto&, const auto&) {};
    std::uint64_t accepted = 0;
    for (int iteration = 0; iteration < 200000; ++iteration) {
        std::uint8_t buffer[120];
        const std::size_t size = next() % sizeof(buffer);
        for (std::size_t index = 0; index < size; ++index) {
            const std::uint64_t value = next();
            buffer[index] = static_cast<std::uint8_t>(value) | ((value >> 8) % 3 ? 0x80 : 0);
        }
        Reader r{buffer, buffer + size};
        std::uint64_t pmap = next();
        bool ok = false;
#define ASTS_PLAIN(Type)                                                                                               \
    case __LINE__: {                                                                                                   \
        Type m;                                                                                                        \
        ok = decode(r, pmap, m);                                                                                       \
        break;                                                                                                         \
    }
#define ASTS_GROUPS(Type)                                                                                              \
    case __LINE__: {                                                                                                   \
        Type m;                                                                                                        \
        ok = decode(r, pmap, m, ignore);                                                                               \
        break;                                                                                                         \
    }
        constexpr int kFirst = __LINE__ + 2;
        switch (kFirst + static_cast<int>(next() % 18)) {
            ASTS_PLAIN(Logon)
            ASTS_PLAIN(Logout)
            ASTS_PLAIN(Heartbeat)
            ASTS_PLAIN(SecurityStatus)
            ASTS_PLAIN(TradingSessionStatus)
            ASTS_GROUPS(SecurityDefinition)
            ASTS_GROUPS(W_Generic)
            ASTS_GROUPS(X_Generic)
            ASTS_GROUPS(W_OLS_FOND)
            ASTS_GROUPS(W_OLS_CURR)
            ASTS_GROUPS(W_TLS_FOND)
            ASTS_GROUPS(W_TLS_CURR)
            ASTS_GROUPS(X_MSR_FOND)
            ASTS_GROUPS(X_MSR_CURR)
            ASTS_GROUPS(X_OLR_FOND)
            ASTS_GROUPS(X_OLR_CURR)
            ASTS_GROUPS(X_TLR_FOND)
            ASTS_GROUPS(X_TLR_CURR)
        }
        REQUIRE(r.p <= r.end && (!ok || r.ok));
        accepted += ok;
    }
    REQUIRE(accepted > 0);
}

using OrderMap = std::map<std::string, std::pair<char, std::pair<std::int64_t, std::int64_t>>>; // id -> side, price, size

struct BookSink : StreamSink<X_OLR_CURR, W_OLS_CURR> {
    OrderBook book{64}; // small on purpose: growth is part of the test
    std::vector<std::string> names;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> losses;
    int all_stale{}, clears{}, rejected{};

    void on_loss(std::uint32_t first, std::uint32_t last) {
        losses.emplace_back(first, last);
    }

    void on_instrument(std::uint32_t slot, std::string_view symbol, std::string_view board) {
        REQUIRE(slot == names.size());
        names.push_back(std::string(symbol) + "|" + std::string(board));
    }
    void on_clear(std::uint32_t slot) {
        book.clear(slot);
        ++clears;
    }
    void on_snapshot(std::uint32_t slot, const W_OLS_CURR&, const SnapshotEntry& e) {
        rejected += !book.apply_snapshot(slot, e);
    }
    void on_incremental(std::uint32_t slot, const X_OLR_CURR&, const IncrementalEntry& e) {
        rejected += !book.apply(slot, e);
    }
    void on_all_stale() {
        ++all_stale;
    }
    [[nodiscard]] OrderMap orders(std::uint32_t slot) const {
        OrderMap out;
        book.for_each(slot, [&](const Order& order) {
            out[std::string(order.entry_id())] = {order.side, {order.price.mantissa, order.size.mantissa}};
        });
        REQUIRE(out.size() == book.orders(slot));
        return out;
    }
};

using TestStream = CurrencyOrders<BookSink>;

void feed(TestStream& stream, const Bytes& packet, std::uint64_t now = 0) {
    stream.on_incremental(packet.data(), packet.size(), now);
}
void snap(TestStream& stream, const Bytes& packet) {
    stream.on_snapshot(packet.data(), packet.size());
}

void order_book_operations() {
    OrderBook book(8);
    std::map<std::pair<std::uint32_t, std::string>, std::int64_t> reference;
    std::uint64_t state = 99;
    const auto next = [&](std::uint64_t bound) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state % bound;
    };
    X_OLR_CURR::GroupMDEntries entry{};
    for (int step = 0; step < 60000; ++step) {
        const auto slot = static_cast<std::uint32_t>(next(5));
        const std::string id = std::to_string(next(900));
        const auto found = reference.find({slot, id});
        const std::uint8_t type[1] = {static_cast<std::uint8_t>(('0' + next(2)) | 0x80)};
        std::vector<std::uint8_t> wire(id.begin(), id.end());
        wire.back() |= 0x80;
        entry.MDEntryType = {{type, 1}, true};
        entry.MDEntryID = {{wire.data(), static_cast<std::uint32_t>(wire.size())}, true};
        const auto size = static_cast<std::int64_t>(1 + next(1000));
        entry.MDEntryPx = {{static_cast<std::int64_t>(next(5000)), -2}, true};
        if (next(40) == 0) {
            book.clear(slot);
            for (auto it = reference.begin(); it != reference.end();)
                it = it->first.first == slot ? reference.erase(it) : std::next(it);
        } else if (found == reference.end() || next(3) == 0) {
            entry.MDUpdateAction = {static_cast<std::uint32_t>(found == reference.end() ? next(2) * 2 : next(2)), true};
            entry.MDEntrySize = {{size, 0}, true};
            const bool applied = book.apply(slot, entry);
            if (found == reference.end() && entry.MDUpdateAction.value == 2)
                REQUIRE(!applied); // deleting an unknown order is reported, not invented
            else {
                REQUIRE(applied);
                reference[{slot, id}] = size;
            }
        } else {
            entry.MDUpdateAction = {2, true};
            entry.MDEntrySize = {{}, false};
            REQUIRE(book.apply(slot, entry));
            reference.erase(found);
        }
        if (step % 997 == 0) {
            REQUIRE(book.orders() == reference.size());
            for (const auto& [key, expected] : reference) {
                const Order* order = book.find(key.first, key.second);
                REQUIRE(order && order->size.mantissa == expected && order->entry_id() == key.second);
            }
        }
    }
    REQUIRE(book.orders() == reference.size() && book.find(0, "no-such-order") == nullptr);
}

// Joining mid-day: an active instrument syncs from a mid-cycle snapshot because the
// entries after it are all held; a quiet one waits for a cycle begun after we started.
void late_join() {
    BookSink sink;
    TestStream stream(sink);
    feed(stream, asts_test::orders(500, {add("USD", "11", 41, '0', 9100, 5)}));
    feed(stream, asts_test::orders(501, {add("USD", "12", 42, '1', 9200, 7)}));
    REQUIRE(stream.needs_snapshot() && stream.unsynced() == 1 && sink.book.orders() == 0);

    // Mid-cycle snapshots. USD as of RptSeq 41: entry 42 is held, so it is usable now.
    snap(stream, asts_test::order_snapshot(300, "USD", "CETS", 41, {{'0', "9", {9000, -2}, {3, 0}}, {'0', "11", {9100, -2}, {5, 0}}}));
    // EUR has nothing queued and this cycle began before we did: not yet trustworthy.
    snap(stream, asts_test::order_snapshot(301, "EUR", "CETS", 17, {{'1', "70", {9900, -2}, {1, 0}}}));
    stream.on_timer(1'000'000'000);
    const std::uint32_t usd = stream.find("USD", "CETS"), eur = stream.find("EUR", "CETS");
    REQUIRE(stream.synced(usd) && !stream.synced(eur) && stream.rpt_seq(usd) == 42);
    REQUIRE((sink.orders(usd) == OrderMap{{"9", {'0', {9000, 3}}}, {"11", {'0', {9100, 5}}}, {"12", {'1', {9200, 7}}}}));
    REQUIRE(stream.stats().snapshots_too_old == 1 && sink.book.orders(eur) == 0);

    // The next cycle starts at 1 and names every instrument, including an empty one.
    snap(stream, asts_test::order_snapshot(1, "USD", "CETS", 42, {{'0', "9", {9000, -2}, {3, 0}}}));
    snap(stream, asts_test::order_snapshot(2, "EUR", "CETS", 17, {{'1', "70", {9900, -2}, {1, 0}}}));
    snap(stream, asts_test::order_snapshot(3, "CNY", "CETS", 0, {}));
    snap(stream, asts_test::heartbeat(4));
    REQUIRE(stream.needs_snapshot()); // the cycle is not known to be complete until the next one begins
    snap(stream, asts_test::order_snapshot(1, "USD", "CETS", 42, {}));
    REQUIRE(stream.unsynced() == 0 && !stream.needs_snapshot() && stream.instruments() == 3);
    REQUIRE((sink.orders(eur) == OrderMap{{"70", {'1', {9900, 1}}}}) && sink.book.orders(usd) == 3);

    feed(stream, asts_test::orders(502, {remove("USD", "11", 43, '0'), add("CNY", "1", 1, '0', 1250, 9)}));
    REQUIRE(sink.book.orders(usd) == 2 && sink.book.orders(stream.find("CNY", "CETS")) == 1 && sink.rejected == 0);
}

TestStream& synced(BookSink& sink, TestStream& stream, std::uint32_t first_seq = 99) {
    feed(stream, asts_test::heartbeat(first_seq));
    snap(stream, asts_test::heartbeat(40));
    snap(stream, asts_test::order_snapshot(1, "USD", "CETS", 5, {{'0', "1", {9000, -2}, {1, 0}}}));
    snap(stream, asts_test::order_snapshot(2, "EUR", "CETS", 8, {{'1', "2", {9900, -2}, {2, 0}}}));
    snap(stream, asts_test::order_snapshot(1, "USD", "CETS", 5, {{'0', "1", {9000, -2}, {1, 0}}}));
    REQUIRE(stream.unsynced() == 0 && !stream.needs_snapshot() && sink.book.orders() == 2);
    return stream;
}

// After a loss nothing is discarded: an instrument whose next entry is the next
// RptSeq carries on, a quiet one is confirmed by a later cycle, and only the one
// that really missed entries is rebuilt.
void loss_confirms_or_rebuilds() {
    BookSink sink;
    StreamConfig config;
    config.reorder_timeout_ns = 1000;
    TestStream stream(sink, config);
    synced(sink, stream);
    feed(stream, asts_test::orders(100, {add("USD", "3", 6, '0', 9010, 1)}), 10);
    // 101 is lost on both feeds; it carried EUR's RptSeq 9.
    feed(stream, asts_test::orders(102, {add("USD", "4", 7, '0', 9020, 1)}), 20);
    REQUIRE(stream.unsynced() == 0 && sink.all_stale == 0);
    stream.on_timer(5000);
    REQUIRE(sink.all_stale == 1 && stream.stats().losses == 1 && sink.clears == 2);
    const std::uint32_t usd = stream.find("USD", "CETS"), eur = stream.find("EUR", "CETS");
    // USD's held entry was the very next one: it resumed without a snapshot and without being cleared.
    REQUIRE(stream.synced(usd) && sink.book.orders(usd) == 3 && !stream.synced(eur) && stream.needs_snapshot());
    REQUIRE(stream.stats().resumed_without_snapshot == 1);

    // A snapshot from the cycle already in flight proves nothing about EUR.
    snap(stream, asts_test::order_snapshot(7, "EUR", "CETS", 8, {{'1', "2", {9900, -2}, {2, 0}}}));
    REQUIRE(!stream.synced(eur));
    // EUR's next entry jumps from 8 to 10: it did miss one.
    feed(stream, asts_test::orders(103, {add("EUR", "6", 10, '1', 9950, 4)}), 6000);
    REQUIRE(!stream.synced(eur) && sink.book.orders(eur) == 1);
    // The snapshot as of 9 covers the hole and the held entry 10 follows it.
    snap(stream, asts_test::order_snapshot(1, "USD", "CETS", 7, {}));
    snap(stream, asts_test::order_snapshot(2, "EUR", "CETS", 9, {{'1', "5", {9940, -2}, {3, 0}}}));
    stream.on_timer(1'000'000'000);
    REQUIRE(stream.synced(eur) && (sink.orders(eur) == OrderMap{{"5", {'1', {9940, 3}}}, {"6", {'1', {9950, 4}}}}));
    REQUIRE(sink.clears == 3 && sink.rejected == 0);

    // A second loss with both instruments quiet: the next whole cycle confirms them untouched.
    feed(stream, asts_test::heartbeat(9000), 2'000'000'000);
    REQUIRE(sink.all_stale == 2 && stream.unsynced() == 2);
    snap(stream, asts_test::order_snapshot(1, "USD", "CETS", 7, {}));
    snap(stream, asts_test::order_snapshot(2, "EUR", "CETS", 10, {}));
    REQUIRE(stream.unsynced() == 0 && stream.stats().confirmed_by_snapshot == 2 && sink.clears == 3);
    REQUIRE(sink.book.orders(usd) == 3 && sink.book.orders(eur) == 2);
}

// A snapshot counts only when seen whole, from RouteFirst to LastFragment.
void fragmented_snapshots() {
    BookSink sink;
    TestStream stream(sink);
    feed(stream, asts_test::heartbeat(10));
    snap(stream, asts_test::heartbeat(90));
    snap(stream, asts_test::order_snapshot(1, "USD", "CETS", 5, {{'0', "1", {9000, -2}, {1, 0}}}, true, false));
    snap(stream, asts_test::order_snapshot(2, "USD", "CETS", 5, {{'0', "2", {9010, -2}, {1, 0}}}, false, false));
    snap(stream, asts_test::order_snapshot(3, "USD", "CETS", 5, {{'1', "3", {9100, -2}, {1, 0}}}, false, true));
    // EUR's first fragment (4) is lost on both feeds; its tail alone must not restore it.
    for (int copy = 0; copy < 8; ++copy)
        snap(stream, asts_test::order_snapshot(5, "EUR", "CETS", 8, {{'1', "9", {9900, -2}, {2, 0}}}, false, true));
    stream.on_timer(1'000'000'000);
    const std::uint32_t usd = stream.find("USD", "CETS"), eur = stream.find("EUR", "CETS");
    REQUIRE(stream.synced(usd) && sink.book.orders(usd) == 3 && !stream.synced(eur) && sink.book.orders(eur) == 0);
    snap(stream, asts_test::order_snapshot(1, "USD", "CETS", 5, {}));
    REQUIRE(stream.needs_snapshot()); // the broken cycle does not count as complete
    snap(stream, asts_test::order_snapshot(2, "EUR", "CETS", 8, {{'1', "8", {9890, -2}, {1, 0}}}, true, false));
    snap(stream, asts_test::order_snapshot(3, "EUR", "CETS", 8, {{'1', "9", {9900, -2}, {2, 0}}}, false, true));
    snap(stream, asts_test::order_snapshot(1, "USD", "CETS", 5, {}));
    REQUIRE(stream.unsynced() == 0 && sink.book.orders(eur) == 2 && !stream.needs_snapshot());
}

void duplicates_reordering_and_garbage() {
    BookSink sink;
    TestStream stream(sink);
    synced(sink, stream, 70099);
    const Bytes p100 = asts_test::orders(70100, {add("USD", "3", 6, '0', 9010, 1)});
    const Bytes p101 = asts_test::orders(70101, {add("USD", "4", 7, '0', 9020, 1)});
    const Bytes p102 = asts_test::orders(70102, {remove("USD", "3", 8, '0')});
    feed(stream, p100);
    feed(stream, p100); // feed B
    feed(stream, p102, 1000);
    feed(stream, p101, 1100);
    feed(stream, p102, 1200);
    const std::uint32_t usd = stream.find("USD", "CETS");
    REQUIRE(stream.next_sequence() == 70103 && sink.book.orders(usd) == 2 && stream.rpt_seq(usd) == 8);
    REQUIRE(stream.arbiter().duplicates == 2 && stream.arbiter().reordered == 1 && sink.all_stale == 0);

    // The gate restarting its numbering is a fresh start: RptSeq 1 opens each instrument again.
    for (std::uint32_t seq = 1; seq <= 200; ++seq)
        feed(stream, asts_test::heartbeat(seq));
    REQUIRE(stream.stats().restarts == 1 && sink.all_stale == 1 && stream.unsynced() == 2);
    feed(stream, asts_test::orders(stream.next_sequence(), {add("USD", "1", 1, '1', 9500, 2)}));
    REQUIRE(stream.synced(usd) && (sink.orders(usd) == OrderMap{{"1", {'1', {9500, 2}}}}));

    const Bytes runt{1, 0, 0};
    feed(stream, runt);
    Bytes corrupt = asts_test::orders(stream.next_sequence(), {add("USD", "2", 2, '0', 9000, 1)});
    corrupt.resize(corrupt.size() - 2);
    feed(stream, corrupt);
    REQUIRE(stream.stats().malformed == 2 && sink.all_stale == 2);
}

// Reference model through a lossy, duplicating, reordering A/B channel, with snapshot
// cycles shaped like the gate's: generated at one instant, trickled out while
// incrementals keep flowing, every instrument named. Whenever the stream calls an
// instrument synced, its orders must equal the truth as of the RptSeq the stream reports.
// With TCP replay, each lost range is handed back a little later, or reported as
// unobtainable, the way a replay request would end.
void randomized_lossy_channel(std::uint64_t seed, bool with_replay) {
    std::uint64_t state = seed * 0x9E3779B97F4A7C15ull + 1;
    const auto next = [&](std::uint64_t bound) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state % bound;
    };
    static const char* const kSymbols[] = {"USD", "EUR", "CNY", "GLD", "KZT", "BYN"};
    constexpr std::size_t kInstruments = std::size(kSymbols);
    constexpr std::uint32_t kPackets = 5000;
    constexpr std::uint32_t kQuietFrom = 4300; // loss stops here so everything can settle
    struct Truth {
        OrderMap orders;
        std::int32_t rpt_seq{};
        std::vector<OrderMap> history{OrderMap{}}; // state after each RptSeq
        std::vector<std::pair<std::uint32_t, std::uint32_t>> published; // packet seq -> RptSeq after it
    };
    std::vector<Truth> truth(kInstruments);
    BookSink sink;
    StreamConfig config;
    config.reorder_window = 16;
    config.reorder_timeout_ns = 800;
    config.replay_interval_ns = 300;
    config.tcp_replay = with_replay;
    TestStream stream(sink, config);
    std::map<std::uint32_t, Bytes> sent; // what a replay request can return
    struct Request {
        std::uint32_t first, last, due;
    };
    std::vector<Request> requests;
    std::size_t losses_seen = 0;
    std::uint64_t now = 0;
    std::uint32_t snapshot_seq = 1, next_order_id = 1;
    std::deque<Bytes> delayed, cycle;

    const auto verify = [&] {
        for (std::uint32_t slot = 0; slot < stream.instruments(); ++slot) {
            if (!stream.synced(slot))
                continue;
            const std::string symbol(stream.symbol(slot));
            const std::size_t index = static_cast<std::size_t>(
                std::find_if(std::begin(kSymbols), std::end(kSymbols), [&](const char* s) { return symbol == s; }) -
                std::begin(kSymbols));
            const std::uint32_t rpt_seq = stream.rpt_seq(slot);
            // Being in sync means holding everything published up to the stream's position,
            // not merely a state that was right at some earlier point.
            std::uint32_t owed = 0;
            if (index < kInstruments)
                for (auto it = truth[index].published.rbegin(); it != truth[index].published.rend(); ++it)
                    if (it->first < stream.next_sequence()) {
                        owed = it->second;
                        break;
                    }
            if (index >= kInstruments || rpt_seq < owed || rpt_seq >= truth[index].history.size() ||
                sink.orders(slot) != truth[index].history[rpt_seq])
                throw std::runtime_error("order mismatch: seed " + std::to_string(seed) + (with_replay ? " (replay) " : " ") +
                                         symbol + " holds RptSeq " + std::to_string(rpt_seq) + ", owes " +
                                         std::to_string(owed) + " at packet " + std::to_string(stream.next_sequence() - 1));
        }
    };
    const auto send_incremental = [&](const Bytes& packet, bool lossy) {
        for (int copy = 0; copy < 2; ++copy) { // feeds A and B
            if (lossy && next(100) < 10)
                continue;
            if (lossy && next(100) < 8) {
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

    for (std::uint32_t seq = 1; seq <= kPackets; ++seq) {
        const bool lossy = seq < kQuietFrom;
        now += 100;
        std::vector<asts_test::Order> entries;
        if (next(25) != 0) { // otherwise a heartbeat
            const std::size_t count = 1 + next(4);
            std::string last_symbol;
            for (std::size_t n = 0; n < count; ++n) {
                // BYN stays untouched for a long stretch, KZT forever: quiet instruments have their own rules.
                const std::size_t index = next(seq < 1500 ? kInstruments - 2 : kInstruments - 1);
                Truth& inst = truth[index];
                asts_test::Order entry;
                entry.rpt_seq = ++inst.rpt_seq;
                // Use the copy operator the way the gate does: repeat what has not changed.
                if (kSymbols[index] != last_symbol) {
                    entry.symbol = kSymbols[index];
                    entry.board = "CETS";
                    last_symbol = kSymbols[index];
                }
                if (n == 0 && !entry.board)
                    entry.board = "CETS";
                const std::uint64_t choice = next(10);
                if (inst.orders.empty() || (choice < 5 && inst.orders.size() < 30)) { // keep books small
                    entry.id = std::to_string(next_order_id++);
                    entry.action = 0;
                    entry.type = next(2) ? '0' : '1';
                    entry.price = Dec{9000 + static_cast<std::int64_t>(next(300)), -2};
                    entry.size = Dec{1 + static_cast<std::int64_t>(next(50)), 0};
                    inst.orders[entry.id] = {*entry.type, {entry.price->first, entry.size->first}};
                } else {
                    auto victim = inst.orders.begin();
                    std::advance(victim, static_cast<std::ptrdiff_t>(next(inst.orders.size())));
                    entry.id = victim->first;
                    entry.type = victim->second.first;
                    entry.price = Dec{victim->second.second.first, -2};
                    if (choice < 7) {
                        entry.action = 1;
                        entry.size = Dec{1 + static_cast<std::int64_t>(next(50)), 0};
                        victim->second.second.second = entry.size->first;
                    } else {
                        entry.action = 2;
                        entry.null_size = true;
                        inst.orders.erase(victim);
                    }
                }
                inst.history.push_back(inst.orders);
                inst.published.emplace_back(seq, static_cast<std::uint32_t>(inst.rpt_seq));
                entries.push_back(entry);
            }
            sent[seq] = asts_test::orders(seq, entries);
        } else {
            sent[seq] = asts_test::heartbeat(seq);
        }
        send_incremental(sent[seq], lossy);
        stream.on_timer(now);
        verify();
        for (; losses_seen < sink.losses.size(); ++losses_seen)
            requests.push_back({sink.losses[losses_seen].first, sink.losses[losses_seen].second,
                                seq + static_cast<std::uint32_t>(next(30))});
        for (std::size_t i = 0; i < requests.size();) {
            if (requests[i].due > seq && seq != kPackets) {
                ++i;
                continue;
            }
            const bool ok = next(5) != 0;
            for (std::uint32_t lost = requests[i].first; ok && lost <= requests[i].last; ++lost) {
                stream.on_recovered(sent[lost].data() + kPreambleSize, sent[lost].size() - kPreambleSize);
                verify();
            }
            stream.replay_finished(requests[i].first, requests[i].last, ok);
            verify();
            requests.erase(requests.begin() + static_cast<std::ptrdiff_t>(i));
        }

        if (cycle.empty()) {
            // A whole cycle as of this instant: every instrument, empty ones as Empty Book.
            snapshot_seq = 1;
            for (std::size_t index = 0; index < kInstruments; ++index) {
                std::vector<asts_test::SnapshotOrder> orders;
                for (const auto& [id, order] : truth[index].orders)
                    orders.push_back({order.first, id, {order.second.first, -2}, {order.second.second, 0}});
                if (orders.size() > 2 && next(2) == 0) {
                    const auto middle = orders.begin() + static_cast<std::ptrdiff_t>(orders.size() / 2);
                    cycle.push_back(asts_test::order_snapshot(snapshot_seq++, kSymbols[index], "CETS", truth[index].rpt_seq,
                                                              {orders.begin(), middle}, true, false));
                    cycle.push_back(asts_test::order_snapshot(snapshot_seq++, kSymbols[index], "CETS", truth[index].rpt_seq,
                                                              {middle, orders.end()}, false, true));
                } else {
                    cycle.push_back(asts_test::order_snapshot(snapshot_seq++, kSymbols[index], "CETS", truth[index].rpt_seq, orders));
                }
            }
            for (std::uint64_t pause = next(3); pause > 0; --pause)
                cycle.push_back(asts_test::heartbeat(snapshot_seq++));
        }
        for (int copy = 0; copy < 2; ++copy)
            if (!lossy || next(100) >= 10)
                snap(stream, cycle.front());
        cycle.pop_front();
        stream.on_timer(now);
        verify();
    }
    const StreamStats& stats = stream.stats();
    REQUIRE(stats.losses > 0 && stats.snapshots_applied > 0 && stats.replayed > 0 && stats.malformed == 0);
    REQUIRE(stats.resumed_without_snapshot > 0 && sink.rejected == 0);
    REQUIRE(with_replay ? stats.recovered_packets > 0 && stats.confirmed_by_replay > 0 && stats.replay_ranges_failed > 0
                        : stats.confirmed_by_snapshot > 0 && sink.losses.empty());
    REQUIRE(stream.unsynced() == 0 && !stream.needs_snapshot() && stream.instruments() == kInstruments);
    for (std::uint32_t slot = 0; slot < stream.instruments(); ++slot) {
        const std::string symbol(stream.symbol(slot));
        const std::size_t index = static_cast<std::size_t>(
            std::find_if(std::begin(kSymbols), std::end(kSymbols), [&](const char* s) { return symbol == s; }) -
            std::begin(kSymbols));
        REQUIRE(stream.rpt_seq(slot) == static_cast<std::uint32_t>(truth[index].rpt_seq));
        REQUIRE(sink.orders(slot) == truth[index].orders);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        copy_operator_decoding();
        random_input();
        order_book_operations();
        late_join();
        loss_confirms_or_rebuilds();
        fragmented_snapshots();
        duplicates_reordering_and_garbage();
        const std::uint64_t seeds = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 25;
        for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
            randomized_lossy_channel(seed, false);
            randomized_lossy_channel(seed, true);
        }
    } catch (const std::exception& error) {
        std::cerr << "asts_stream_test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
