#pragma once

// Order-by-order book for the ASTS OLR/OLS streams: every resting order the
// gate publishes, keyed by instrument slot and MDEntryID. One object holds all
// instruments of a stream. Orders live in a pooled array, found through an
// open-addressing index and chained per instrument, so add, change and delete
// are O(1) and clearing an instrument costs only its own orders. Memory grows
// only when the number of live orders reaches a new high.
//
// MDEntryID identifies the published order entry; the guide notes it is not
// the exchange order number seen in trading interfaces.

#include "moex/fast/fast_wire.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace moex::fast::asts {

struct Order {
    static constexpr std::size_t kMaxId = 23;

    std::array<char, kMaxId + 1> id{}; // zero-padded MDEntryID
    Decimal price{};
    Decimal size{};
    std::uint32_t slot{};
    char side{};   // MDEntryType: '0' bid, '1' offer, 'f'/'g' closing-auction market orders
    char status{}; // OrderStatus: 'O' active, 'T' not yet activated

    [[nodiscard]] std::string_view entry_id() const noexcept {
        return {id.data(), ::strnlen(id.data(), kMaxId)};
    }
};

class OrderBook {
  public:
    explicit OrderBook(std::size_t expected_orders = 1 << 16) {
        std::size_t buckets = 64;
        while (buckets < expected_orders * 2)
            buckets <<= 1;
        index_.assign(buckets, kNone);
        nodes_.reserve(expected_orders);
    }

    // Applies an OLR entry. Returns false when the entry could not be applied as
    // stated: an id too long to hold, a change or delete of an unknown order.
    template <class Entry> bool apply(std::uint32_t slot, const Entry& e) {
        const char type = e.MDEntryType.value.front();
        if (type == 'J') {
            clear(slot);
            return true;
        }
        std::array<char, Order::kMaxId + 1> id{};
        if (!e.MDEntryID.present || e.MDEntryID.value.size() > Order::kMaxId)
            return false;
        e.MDEntryID.value.copy(id.data(), Order::kMaxId);
        const std::uint32_t action = e.MDUpdateAction.value_or(0);
        std::size_t bucket = find_bucket(slot, id);
        if (action == 2) {
            if (index_[bucket] == kNone)
                return false;
            erase(bucket);
            return true;
        }
        if (index_[bucket] == kNone) {
            if (action == 1)
                return false;
            bucket = insert(bucket, slot, id);
        }
        Order& order = nodes_[index_[bucket]].order;
        order.side = type;
        if (e.MDEntryPx.present)
            order.price = e.MDEntryPx.value;
        if (e.MDEntrySize.present)
            order.size = e.MDEntrySize.value;
        if (e.OrderStatus.present)
            order.status = e.OrderStatus.value.front();
        return true;
    }

    // Applies an OLS snapshot entry; clear(slot) first.
    template <class Entry> bool apply_snapshot(std::uint32_t slot, const Entry& e) {
        const char type = e.MDEntryType.value.front();
        if (type == 'J')
            return true;
        std::array<char, Order::kMaxId + 1> id{};
        if (!e.MDEntryID.present || e.MDEntryID.value.size() > Order::kMaxId)
            return false;
        e.MDEntryID.value.copy(id.data(), Order::kMaxId);
        std::size_t bucket = find_bucket(slot, id);
        if (index_[bucket] == kNone)
            bucket = insert(bucket, slot, id);
        Order& order = nodes_[index_[bucket]].order;
        order.side = type;
        order.price = e.MDEntryPx.value;
        order.size = e.MDEntrySize.value;
        order.status = e.OrderStatus.present ? e.OrderStatus.value.front() : '\0';
        return true;
    }

    void clear(std::uint32_t slot) {
        if (slot >= heads_.size())
            return;
        while (heads_[slot] != kNone)
            erase(find_bucket(slot, nodes_[heads_[slot]].order.id));
    }

    void clear_all() {
        index_.assign(index_.size(), kNone);
        heads_.assign(heads_.size(), kNone);
        counts_.assign(counts_.size(), 0);
        nodes_.clear();
        free_ = kNone;
        live_ = 0;
    }

    [[nodiscard]] const Order* find(std::uint32_t slot, std::string_view entry_id) const noexcept {
        std::array<char, Order::kMaxId + 1> id{};
        if (entry_id.size() > Order::kMaxId)
            return nullptr;
        std::memcpy(id.data(), entry_id.data(), entry_id.size());
        const std::uint32_t node = index_[find_bucket(slot, id)];
        return node == kNone ? nullptr : &nodes_[node].order;
    }

    // Calls visit(const Order&) for every order of the instrument, in no particular order.
    template <class Visitor> void for_each(std::uint32_t slot, Visitor&& visit) const {
        for (std::uint32_t node = slot < heads_.size() ? heads_[slot] : kNone; node != kNone; node = nodes_[node].next)
            visit(nodes_[node].order);
    }

    [[nodiscard]] std::size_t orders(std::uint32_t slot) const noexcept {
        return slot < counts_.size() ? counts_[slot] : 0;
    }
    [[nodiscard]] std::size_t orders() const noexcept {
        return live_;
    }

  private:
    static constexpr std::uint32_t kNone = UINT32_MAX;
    struct Node {
        Order order;
        std::uint32_t previous{kNone};
        std::uint32_t next{kNone};
    };

    static std::size_t hash(std::uint32_t slot, const std::array<char, Order::kMaxId + 1>& id) noexcept {
        std::uint64_t words[3];
        std::memcpy(words, id.data(), sizeof(words));
        std::uint64_t h = (words[0] ^ slot) * 0x9E3779B97F4A7C15ull;
        h = (h ^ (h >> 29) ^ words[1]) * 0xBF58476D1CE4E5B9ull;
        h = (h ^ (h >> 32) ^ words[2]) * 0x94D049BB133111EBull;
        return static_cast<std::size_t>(h >> 20);
    }

    // Bucket holding the order, or the empty bucket where it would go.
    [[nodiscard]] std::size_t find_bucket(std::uint32_t slot, const std::array<char, Order::kMaxId + 1>& id) const noexcept {
        const std::size_t mask = index_.size() - 1;
        std::size_t bucket = hash(slot, id) & mask;
        while (index_[bucket] != kNone) {
            const Order& order = nodes_[index_[bucket]].order;
            if (order.slot == slot && order.id == id)
                break;
            bucket = (bucket + 1) & mask;
        }
        return bucket;
    }

    std::size_t insert(std::size_t bucket, std::uint32_t slot, const std::array<char, Order::kMaxId + 1>& id) {
        if ((live_ + 1) * 2 > index_.size()) {
            grow();
            bucket = find_bucket(slot, id);
        }
        std::uint32_t node = free_;
        if (node != kNone) {
            free_ = nodes_[node].next;
        } else {
            node = static_cast<std::uint32_t>(nodes_.size());
            nodes_.emplace_back();
        }
        if (slot >= heads_.size()) {
            heads_.resize(slot + 1, kNone);
            counts_.resize(slot + 1, 0);
        }
        Node& fresh = nodes_[node];
        fresh.order = Order{};
        fresh.order.id = id;
        fresh.order.slot = slot;
        fresh.previous = kNone;
        fresh.next = heads_[slot];
        if (fresh.next != kNone)
            nodes_[fresh.next].previous = node;
        heads_[slot] = node;
        ++counts_[slot];
        ++live_;
        index_[bucket] = node;
        return bucket;
    }

    void erase(std::size_t bucket) {
        const std::uint32_t node = index_[bucket];
        Node& gone = nodes_[node];
        if (gone.previous != kNone)
            nodes_[gone.previous].next = gone.next;
        else
            heads_[gone.order.slot] = gone.next;
        if (gone.next != kNone)
            nodes_[gone.next].previous = gone.previous;
        --counts_[gone.order.slot];
        --live_;
        gone.next = free_;
        free_ = node;
        // Backward-shift deletion keeps probe chains intact without tombstones.
        const std::size_t mask = index_.size() - 1;
        std::size_t hole = bucket;
        for (std::size_t next = (hole + 1) & mask; index_[next] != kNone; next = (next + 1) & mask) {
            const Order& order = nodes_[index_[next]].order;
            const std::size_t home = hash(order.slot, order.id) & mask;
            if (((next - home) & mask) >= ((next - hole) & mask)) {
                index_[hole] = index_[next];
                hole = next;
            }
        }
        index_[hole] = kNone;
    }

    void grow() {
        index_.assign(index_.size() * 2, kNone);
        const std::size_t mask = index_.size() - 1;
        for (std::uint32_t head : heads_)
            for (std::uint32_t node = head; node != kNone; node = nodes_[node].next) {
                std::size_t bucket = hash(nodes_[node].order.slot, nodes_[node].order.id) & mask;
                while (index_[bucket] != kNone)
                    bucket = (bucket + 1) & mask;
                index_[bucket] = node;
            }
    }

    std::vector<Node> nodes_;
    std::vector<std::uint32_t> index_; // node index per bucket
    std::vector<std::uint32_t> heads_; // first node per instrument slot
    std::vector<std::uint32_t> counts_;
    std::uint32_t free_{kNone};
    std::size_t live_{};
};

} // namespace moex::fast::asts
