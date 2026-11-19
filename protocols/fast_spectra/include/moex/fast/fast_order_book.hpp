#pragma once

// Active orders from the ORDERS-LOG stream, one record per order.
//
// Orders are found by MDEntryID through one open-addressed table and chained
// per instrument, so add, change, delete and per-instrument clear are O(1) per
// order with no allocation once the pool has grown to the day's working size.
// As the spec requires for building a book, OTC orders (MDFlags 0x4) and
// technical trades are left out; the caller still sees every entry.

#include "fast_market_stream.hpp"
#include "fast_spectra_messages.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace moex::fast {

class OrderBook {
  public:
    static constexpr std::int64_t kOtcFlag = 0x4;

    struct Order {
        std::int64_t id{};
        Decimal price{};
        std::int64_t size{};
        std::uint32_t slot{};
        std::uint32_t session{}; // ExchangeTradingSessionID
        bool bid{};

      private:
        friend class OrderBook;
        std::uint32_t prev{}, next{}; // pool index + 1 within the instrument's chain, 0 = none
    };
    struct Stats {
        std::uint64_t added{}, changed{}, deleted{}, readded{}, unknown_change{}, unknown_delete{};
    };

    explicit OrderBook(std::size_t expected_orders = std::size_t{1} << 16) {
        std::size_t buckets = 64;
        while (buckets < expected_orders * 2)
            buckets <<= 1;
        table_.assign(buckets, 0);
        pool_.reserve(expected_orders);
    }

    // Returns false for entries that do not change the book (trades without an order, OTC, Empty Book).
    bool apply(std::uint32_t slot, const OrdersLogMessage::MDEntries& e) {
        const char type = e.MDEntryType.front();
        if ((type != '0' && type != '1') || !e.MDEntryID.present || (e.MDFlags.value_or(0) & kOtcFlag))
            return false;
        switch (e.MDUpdateAction) {
        case 0:
            if (!e.MDEntryPx.present || !e.MDEntrySize.present)
                return false; // technical trade: no order behind it
            add(slot, e.MDEntryID.value, type == '0', e.MDEntryPx.value, e.MDEntrySize.value,
                e.ExchangeTradingSessionID.value_or(0));
            return true;
        case 1:
            if (Order* order = find_mutable(e.MDEntryID.value)) {
                order->size = e.MDEntrySize.value_or(order->size);
                ++stats_.changed;
            } else {
                ++stats_.unknown_change;
            }
            return true;
        case 2:
            if (!remove(e.MDEntryID.value))
                ++stats_.unknown_delete;
            else
                ++stats_.deleted;
            return true;
        default:
            return false;
        }
    }

    // Snapshot entry; the caller clears the instrument when its snapshot begins.
    bool apply(std::uint32_t slot, const BookMessage& m, const BookMessage::MDEntries& e) {
        const char type = e.MDEntryType.front();
        if ((type != '0' && type != '1') || !e.MDEntryID.present || !e.MDEntryPx.present ||
            (e.MDFlags.value_or(0) & kOtcFlag))
            return false;
        add(slot, e.MDEntryID.value, type == '0', e.MDEntryPx.value, e.MDEntrySize.value_or(0),
            m.ExchangeTradingSessionID);
        return true;
    }

    void clear(std::uint32_t slot) noexcept {
        while (slot < heads_.size() && heads_[slot])
            remove(pool_[heads_[slot] - 1].id);
    }
    // Empty Book for one trading session: drop that session's orders in this instrument.
    void clear_session(std::uint32_t slot, std::uint32_t session) noexcept {
        for (std::uint32_t at = slot < heads_.size() ? heads_[slot] : 0; at;) {
            const Order& order = pool_[at - 1];
            const std::uint32_t next = order.next;
            if (order.session == session) {
                // Removal moves the pool's last order into the freed cell; follow it if it was our next.
                const std::uint32_t last = static_cast<std::uint32_t>(pool_.size());
                remove(order.id);
                at = next == last ? at : next;
            } else {
                at = next;
            }
        }
    }
    void clear_all() noexcept {
        pool_.clear();
        table_.assign(table_.size(), 0);
        heads_.assign(heads_.size(), 0);
        counts_.assign(counts_.size(), 0);
    }

    [[nodiscard]] const Order* find(std::int64_t id) const noexcept {
        const std::size_t mask = table_.size() - 1;
        for (std::size_t bucket = hash(id) & mask; table_[bucket]; bucket = (bucket + 1) & mask)
            if (pool_[table_[bucket] - 1].id == id)
                return &pool_[table_[bucket] - 1];
        return nullptr;
    }
    template <class Visitor> void for_each(std::uint32_t slot, Visitor&& visit) const {
        for (std::uint32_t at = slot < heads_.size() ? heads_[slot] : 0; at; at = pool_[at - 1].next)
            visit(pool_[at - 1]);
    }
    [[nodiscard]] std::size_t count(std::uint32_t slot) const noexcept {
        return slot < counts_.size() ? counts_[slot] : 0;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return pool_.size();
    }
    [[nodiscard]] const Stats& stats() const noexcept {
        return stats_;
    }

  private:
    static std::size_t hash(std::int64_t id) noexcept {
        return static_cast<std::size_t>((static_cast<std::uint64_t>(id) * 0x9E3779B97F4A7C15ull) >> 24);
    }
    Order* find_mutable(std::int64_t id) noexcept {
        return const_cast<Order*>(find(id));
    }
    std::size_t bucket_of(std::int64_t id) const noexcept {
        const std::size_t mask = table_.size() - 1;
        std::size_t bucket = hash(id) & mask;
        while (table_[bucket] && pool_[table_[bucket] - 1].id != id)
            bucket = (bucket + 1) & mask;
        return bucket;
    }

    void add(std::uint32_t slot, std::int64_t id, bool bid, Decimal price, std::int64_t size, std::uint32_t session) {
        if (remove(id))
            ++stats_.readded; // the same id announced twice: the later statement wins
        if (slot >= heads_.size()) {
            heads_.resize(slot + 1, 0);
            counts_.resize(slot + 1, 0);
        }
        if ((pool_.size() + 1) * 2 > table_.size())
            grow();
        Order order;
        order.id = id;
        order.price = price;
        order.size = size;
        order.slot = slot;
        order.session = session;
        order.bid = bid;
        order.next = heads_[slot];
        pool_.push_back(order);
        const auto index = static_cast<std::uint32_t>(pool_.size());
        if (order.next)
            pool_[order.next - 1].prev = index;
        heads_[slot] = index;
        ++counts_[slot];
        table_[bucket_of(id)] = index;
        ++stats_.added;
    }

    bool remove(std::int64_t id) noexcept {
        std::size_t bucket = bucket_of(id);
        const std::uint32_t index = table_[bucket];
        if (!index)
            return false;
        // Unlink from the instrument chain.
        const Order gone = pool_[index - 1];
        if (gone.prev)
            pool_[gone.prev - 1].next = gone.next;
        else
            heads_[gone.slot] = gone.next;
        if (gone.next)
            pool_[gone.next - 1].prev = gone.prev;
        --counts_[gone.slot];
        // Close the hole in the probe sequence (backward-shift deletion).
        const std::size_t mask = table_.size() - 1;
        for (std::size_t next = (bucket + 1) & mask; table_[next]; next = (next + 1) & mask) {
            const std::size_t home = hash(pool_[table_[next] - 1].id) & mask;
            if (((next - home) & mask) >= ((next - bucket) & mask)) {
                table_[bucket] = table_[next];
                bucket = next;
            }
        }
        table_[bucket] = 0;
        // Keep the pool dense: move its last order into the freed cell.
        const auto last = static_cast<std::uint32_t>(pool_.size());
        if (index != last) {
            const Order moved = pool_[last - 1];
            pool_[index - 1] = moved;
            table_[bucket_of(moved.id)] = index;
            if (moved.prev)
                pool_[moved.prev - 1].next = index;
            else
                heads_[moved.slot] = index;
            if (moved.next)
                pool_[moved.next - 1].prev = index;
        }
        pool_.pop_back();
        return true;
    }

    void grow() {
        table_.assign(table_.size() * 2, 0);
        const std::size_t mask = table_.size() - 1;
        for (std::uint32_t index = 1; index <= pool_.size(); ++index) {
            std::size_t bucket = hash(pool_[index - 1].id) & mask;
            while (table_[bucket])
                bucket = (bucket + 1) & mask;
            table_[bucket] = index;
        }
    }

    std::vector<Order> pool_;
    std::vector<std::uint32_t> table_; // pool index + 1, 0 = empty
    std::vector<std::uint32_t> heads_; // per instrument slot: pool index + 1 of its first order
    std::vector<std::uint32_t> counts_;
    Stats stats_;
};

// Ready-made wiring of OrdersLogStream to an OrderBook. Derive to add your own
// handling and call the base first, or use it as is.
struct OrderBookSink : OrdersLogSink {
    OrderBook book;

    void on_book_clear(std::uint32_t slot) noexcept {
        book.clear(slot);
    }
    void on_snapshot(std::uint32_t slot, const Snapshot& m, const Snapshot::MDEntries& e) {
        book.apply(slot, m, e);
    }
    void on_incremental(std::uint32_t slot, const Incremental&, const Incremental::MDEntries& e) {
        book.apply(slot, e);
    }
    void on_market_clear(std::uint32_t slot, const Incremental&, const Incremental::MDEntries& e) noexcept {
        if (e.ExchangeTradingSessionID.present)
            book.clear_session(slot, e.ExchangeTradingSessionID.value);
        else
            book.clear(slot);
    }
    void on_sequence_reset() noexcept {
        book.clear_all();
    }
};

} // namespace moex::fast
