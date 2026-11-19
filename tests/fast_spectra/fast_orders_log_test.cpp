#include "fast_test_encoder.hpp"
#include "moex/fast/fast_order_book.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace moex::fast;
using fast_test::BookOrder;
using fast_test::Bytes;
using fast_test::Dec;
using fast_test::OrderEntry;

namespace {

#define REQUIRE(condition)                                                                                             \
    do {                                                                                                               \
        if (!(condition))                                                                                              \
            throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition);       \
    } while (false)

using Stream = OrdersLogStream<OrderBookSink>;

OrderEntry order(std::uint32_t action, char side, std::int64_t id, std::uint64_t security, std::uint64_t rpt,
                 std::int64_t price, std::int64_t size, std::uint64_t session = 7000) {
    OrderEntry entry;
    entry.action = action;
    entry.type = side;
    entry.id = id;
    entry.security_id = security;
    entry.rpt_seq = rpt;
    entry.price = Dec{price, 0};
    entry.size = size;
    entry.session = session;
    entry.flags = 1;
    return entry;
}
OrderEntry empty_book(std::optional<std::uint64_t> session) {
    OrderEntry entry;
    entry.type = 'J';
    entry.session = session;
    return entry;
}
void feed(Stream& stream, const Bytes& packet) {
    stream.on_incremental(packet.data(), packet.size(), 0);
}
void snap(Stream& stream, const Bytes& packet) {
    stream.on_snapshot(packet.data(), packet.size());
}
std::set<std::int64_t> ids(const OrderBook& book, std::uint32_t slot) {
    std::set<std::int64_t> out;
    book.for_each(slot, [&](const OrderBook::Order& o) { REQUIRE(o.slot == slot && out.insert(o.id).second); });
    REQUIRE(out.size() == book.count(slot));
    return out;
}

// The container against a plain map under random add/change/delete/clear traffic.
void order_book_matches_reference() {
    OrderBook book(4);
    std::map<std::int64_t, std::pair<std::uint32_t, std::int64_t>> reference; // id -> slot, size
    std::uint64_t state = 0x1234567;
    const auto next = [&](std::uint64_t bound) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state % bound;
    };
    for (int step = 0; step < 300000; ++step) {
        const auto id = static_cast<std::int64_t>(next(5000)) * 7919 - 1000;
        const auto slot = static_cast<std::uint32_t>(next(9));
        const std::uint64_t choice = next(100);
        OrdersLogMessage::MDEntries e{};
        const std::uint8_t type[1] = {static_cast<std::uint8_t>((next(2) ? '0' : '1') | 0x80)};
        e.MDEntryType = {type, 1};
        e.MDEntryID = {id, true};
        e.MDEntryPx = {{static_cast<std::int64_t>(next(1000)), -2}, true};
        e.MDEntrySize = {static_cast<std::int64_t>(1 + next(50)), true};
        e.ExchangeTradingSessionID = {static_cast<std::uint32_t>(7000 + next(2)), true};
        if (choice < 45) {
            e.MDUpdateAction = 0;
            REQUIRE(book.apply(slot, e));
            reference[id] = {slot, e.MDEntrySize.value};
        } else if (choice < 65) {
            e.MDUpdateAction = 1;
            book.apply(slot, e);
            if (reference.count(id))
                reference[id].second = e.MDEntrySize.value;
        } else if (choice < 98) {
            e.MDUpdateAction = 2;
            book.apply(slot, e);
            reference.erase(id);
        } else if (choice < 99) {
            book.clear(slot);
            std::erase_if(reference, [&](const auto& item) { return item.second.first == slot; });
        } else {
            const std::uint32_t session = 7000 + static_cast<std::uint32_t>(next(2));
            std::vector<std::int64_t> doomed;
            book.for_each(slot, [&](const OrderBook::Order& o) {
                if (o.session == session)
                    doomed.push_back(o.id);
            });
            book.clear_session(slot, session);
            for (const std::int64_t gone : doomed)
                reference.erase(gone);
        }
        REQUIRE(book.size() == reference.size());
        if (step % 997 == 0) {
            std::size_t chained = 0;
            for (std::uint32_t s = 0; s < 9; ++s)
                chained += ids(book, s).size();
            REQUIRE(chained == reference.size());
            for (const auto& [key, expected] : reference) {
                const OrderBook::Order* found = book.find(key);
                REQUIRE(found && found->slot == expected.first && found->size == expected.second);
            }
        }
    }
    REQUIRE(book.stats().readded > 0 && book.stats().unknown_delete > 0 && book.find(-123456789) == nullptr);
}

void late_join_and_order_lifecycle() {
    OrderBookSink sink;
    Stream stream(sink);
    feed(stream, fast_test::orders_log(50, {order(0, '0', 11, 5, 3, 100, 10)}));
    feed(stream, fast_test::orders_log(51, {order(0, '1', 12, 5, 4, 105, 4)}));
    REQUIRE(stream.unsynced() == 1 && sink.book.size() == 0);
    // Snapshot as of 50 in two fragments; an OTC order in it is not part of the book.
    snap(stream, fast_test::sequence_reset(400, 1));
    snap(stream, fast_test::book_message(1, 5, 3, 50, {{'0', 9, Dec{99, 0}, 7, 1}, {'0', 8, Dec{98, 0}, 1, 4}}, 0, 1));
    snap(stream, fast_test::book_message(2, 5, 3, 50, {{'0', 11, Dec{100, 0}, 10, 1}}, 1, 0));
    snap(stream, fast_test::sequence_reset(3, 1));
    REQUIRE(stream.unsynced() == 0 && ids(sink.book, 0) == (std::set<std::int64_t>{9, 11, 12}));

    // Partial match, full match, an OTC order and a technical trade.
    OrderEntry partial = order(1, '0', 11, 5, 5, 100, 6);
    partial.last_price = Dec{100, 0};
    partial.last_size = 4;
    partial.trade_id = 900;
    OrderEntry otc = order(0, '0', 77, 5, 7, 101, 3);
    otc.flags = 4;
    OrderEntry technical;
    technical.type = '1';
    technical.id = 78;
    technical.security_id = 5;
    technical.rpt_seq = 8;
    technical.last_price = Dec{100, 0};
    technical.last_size = 1;
    technical.trade_id = 901;
    feed(stream, fast_test::orders_log(52, {partial, order(2, '1', 12, 5, 6, 105, 0), otc, technical}));
    REQUIRE(ids(sink.book, 0) == (std::set<std::int64_t>{9, 11}) && sink.book.find(11)->size == 6);
    REQUIRE(sink.book.find(11)->bid && sink.book.find(9)->price == (Decimal{99, 0}) && stream.unsynced() == 0);
    REQUIRE(stream.stats().instrument_gaps == 0);
}

// Empty Book entries name no instrument. Live they hit synced instruments at once;
// for an instrument still recovering they must take effect in feed order during replay.
void market_wide_empty_book() {
    OrderBookSink sink;
    Stream stream(sink);
    feed(stream, fast_test::sequence_reset(1, 1));
    feed(stream, fast_test::orders_log(1, {order(0, '0', 1, 5, 1, 100, 1, 7000), order(0, '0', 2, 5, 2, 101, 1, 7001),
                                           order(0, '1', 3, 6, 1, 200, 1, 7000)}));
    REQUIRE(sink.book.size() == 3 && stream.unsynced() == 0);
    feed(stream, fast_test::orders_log(2, {empty_book(7000)}));
    REQUIRE(ids(sink.book, 0) == (std::set<std::int64_t>{2}) && sink.book.count(1) == 0);
    REQUIRE(stream.stats().market_clears == 1 && stream.unsynced() == 0);

    // Instrument 5 loses sync (RptSeq jump); what follows is queued for it.
    feed(stream, fast_test::orders_log(3, {order(0, '0', 4, 5, 9, 102, 1, 7001), order(0, '1', 5, 6, 2, 201, 1, 7001)}));
    REQUIRE(stream.unsynced() == 1 && sink.book.count(1) == 1);
    feed(stream, fast_test::orders_log(4, {empty_book(7001)}));
    REQUIRE(sink.book.count(1) == 0); // instrument 6 is live and cleared now
    feed(stream, fast_test::orders_log(5, {order(0, '0', 6, 5, 10, 103, 1, 7002)}));
    // Snapshot as of 3 still holds session-7001 orders; the queued Empty Book must remove them.
    snap(stream, fast_test::sequence_reset(90, 1));
    snap(stream, fast_test::book_message(1, 5, 9, 3, {{'0', 2, Dec{101, 0}, 1, 1}, {'0', 4, Dec{102, 0}, 1, 1}}, 1, 1, 7001));
    snap(stream, fast_test::sequence_reset(2, 1));
    REQUIRE(stream.unsynced() == 0 && ids(sink.book, 0) == (std::set<std::int64_t>{6}));

    // Whole-market Empty Book.
    feed(stream, fast_test::orders_log(6, {empty_book({})}));
    REQUIRE(sink.book.size() == 0 && stream.unsynced() == 0);
    feed(stream, fast_test::orders_log(7, {order(0, '0', 7, 5, 11, 104, 2)}));
    REQUIRE(sink.book.size() == 1 && stream.stats().instrument_gaps == 1);
}

// A snapshot newer than a queued Empty Book already reflects it.
void empty_book_older_than_snapshot_is_skipped() {
    OrderBookSink sink;
    Stream stream(sink);
    feed(stream, fast_test::orders_log(20, {order(0, '0', 1, 5, 4, 100, 1, 7000)}));
    feed(stream, fast_test::orders_log(21, {empty_book(7000)}));
    feed(stream, fast_test::orders_log(22, {order(0, '0', 2, 5, 5, 101, 1, 7000)}));
    snap(stream, fast_test::sequence_reset(90, 1));
    snap(stream, fast_test::book_message(1, 5, 5, 22, {{'0', 2, Dec{101, 0}, 1, 1}}));
    snap(stream, fast_test::sequence_reset(2, 1));
    REQUIRE(stream.unsynced() == 0 && ids(sink.book, 0) == (std::set<std::int64_t>{2}));
}

} // namespace

int main() {
    try {
        order_book_matches_reference();
        late_join_and_order_lifecycle();
        market_wide_empty_book();
        empty_book_older_than_snapshot_is_skipped();
    } catch (const std::exception& error) {
        std::cerr << "fast_orders_log_test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
