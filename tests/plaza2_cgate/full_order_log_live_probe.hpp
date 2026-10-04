#pragma once
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "moex/plaza2/cgate/plaza2_decimal.hpp"
#include <algorithm>
#include <deque>
#include <iomanip>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <ostream>

namespace full_order_log_probe {
namespace cg = moex::plaza2::cgate;
using Key = std::pair<std::uint64_t, std::int64_t>; // Native log life and info.trades_rev, never local book revision.
using Levels = std::map<std::tuple<std::int32_t, bool, std::int64_t>, std::int64_t>;
inline void require(bool ok, const char* text) {
    if (!ok)
        throw std::invalid_argument(text);
}
inline std::string digest(const Levels& levels, std::span<const std::int32_t> ids) {
    std::string text = "full_order_log_probe_exact_d16_5_v1\n";
    for (auto id : ids)
        text += "isin=" + std::to_string(id) + "\n";
    for (const auto& [key, quantity] : levels) {
        const auto [isin, bid, price] = key;
        text += std::to_string(isin) + "," + std::to_string(bid) + "," + std::to_string(price) + "," +
                std::to_string(quantity) + "\n";
    }
    return cg::plaza2_sha256_hex(text);
}
struct Window {
    struct Entry {
        std::string hash;
        bool ambiguous{}, paired{};
    };
    std::map<Key, Entry> states[2];
    std::deque<Key> order[2];
    std::size_t capacity;
    std::ostream* output;
    std::uint64_t matched{}, mismatched{}, ambiguous{}, evicted{}, raw_snapshots{}, composite_commits{},
        unresolved_raw_retired{};
    explicit Window(std::size_t cap = 4096, std::ostream* out = nullptr) : capacity(cap), output(out) {
        require(cap > 0 && cap <= 65536, "invalid bounded comparison window");
    }
    void retire(unsigned side) {
        if (side == 1)
            unresolved_raw_retired += std::count_if(states[1].begin(), states[1].end(),
                                                    [](const auto& entry) { return !entry.second.paired; });
        states[side].clear();
        order[side].clear();
    }
    void record(unsigned side, Key key, std::string hash) {
        if (side)
            ++raw_snapshots;
        else
            ++composite_commits;
        auto& own = states[side];
        auto at = own.find(key);
        const bool inserted = at == own.end();
        if (inserted)
            at = own.emplace(key, Entry{hash}).first;
        if (inserted)
            order[side].push_back(key);
        else if (at->second.hash != hash && !at->second.ambiguous) {
            at->second.ambiguous = true;
            ++ambiguous;
        }
        if (own.size() > capacity) {
            if (side == 1 && !own.at(order[side].front()).paired)
                ++unresolved_raw_retired;
            own.erase(order[side].front());
            order[side].pop_front();
            ++evicted;
        }
        const auto other = states[1 - side].find(key);
        if (other == states[1 - side].end() || at->second.ambiguous || other->second.ambiguous || at->second.paired ||
            other->second.paired)
            return;
        at->second.paired = other->second.paired = true;
        const bool equal = at->second.hash == other->second.hash;
        if (equal)
            ++matched;
        else
            ++mismatched;
        if (output)
            *output << "{\"kind\":\"comparison\",\"log_life\":" << key.first << ",\"trades_rev\":" << key.second
                    << ",\"equal\":" << (equal ? "true" : "false")
                    << ",\"composite_sha256\":" << std::quoted(states[0].at(key).hash)
                    << ",\"raw_sha256\":" << std::quoted(states[1].at(key).hash) << "}\n"
                    << std::flush;
    }
};
inline cg::Plaza2RawFieldBinding field(const cg::Plaza2RawTableBinding& table, std::string_view name,
                                       std::string_view type, std::size_t size) {
    for (const auto& f : table.fields)
        if (f.name == name) {
            require(f.type_token == type && f.size == size && f.offset <= table.row_size &&
                        size <= table.row_size - f.offset,
                    "incompatible probe field");
            return f;
        }
    throw std::invalid_argument("missing probe field: " + std::string(name));
}
template <class T> T read(const cg::Plaza2ListenerEvent& e, const cg::Plaza2RawFieldBinding& f) {
    require(f.offset <= e.raw_payload.size() && sizeof(T) <= e.raw_payload.size() - f.offset &&
                (e.raw_nulls.empty() || (f.index < e.raw_nulls.size() && !e.raw_nulls[f.index])),
            "truncated or null probe field");
    return moex::plaza2::public_wire::load<T>(e.raw_payload, f.offset);
}
class Observer final : public cg::Plaza2ListenerEventHandler {
    struct Order {
        std::int32_t isin;
        bool bid;
        std::int64_t price, quantity, revision;
    };
    struct OrderBinding {
        cg::Plaza2RawFieldBinding row, rev, act, isin, dir, price, rest, status;
    };
    Window& window_;
    cg::Plaza2FullOrderLog* book_;
    std::vector<std::int32_t> ids_;
    std::unordered_map<std::int64_t, Order> orders_;
    OrderBinding fields_;
    cg::Plaza2RawFieldBinding publication_, info_rev_, info_life_;
    std::size_t order_index_{}, info_index_{}, order_capacity_;
    Key committed_{}, pending_{};
    bool transaction_{}, info_done_{}, have_info_{}, log_rows_seen_{};
    std::ostream* output_;
    std::uint64_t diagnostic_limit_, diagnostics_{};
    Levels levels() const {
        Levels out;
        if (book_) {
            for (auto isin : ids_)
                for (bool bid : {true, false})
                    for (const auto& [price, quantity] : book_->levels(isin, bid))
                        out[{isin, bid, price}] = quantity;
        } else
            for (const auto& [id, o] : orders_) {
                auto& quantity = out[{o.isin, o.bid, o.price}];
                require(o.quantity <= INT64_MAX - quantity, "reference quantity overflow");
                quantity += o.quantity;
            }
        return out;
    }
    void bind(std::span<const cg::Plaza2RawTableBinding> tables) {
        bool orders = false, info = false;
        for (const auto& t : tables) {
            if (t.name == "orders") {
                order_index_ = t.index;
                orders = true;
                fields_ = {field(t, "replID", "i8", 8),
                           field(t, "replRev", "i8", 8),
                           field(t, "replAct", "i8", 8),
                           field(t, "isin_id", "i4", 4),
                           field(t, "dir", "i1", 1),
                           field(t, "price", "d16.5", 11),
                           field(t, "public_amount_rest", "i8", 8),
                           field(t, "xstatus", "i8", 8)};
            } else if (t.name == "info") {
                info = true;
                info_index_ = t.index;
                publication_ = field(t, "publication_state", "i1", 1);
                info_rev_ = field(t, "trades_rev", "i8", 8);
                info_life_ = field(t, "trades_lifenum", "i8", 8);
            }
        }
        require(orders && info, "probe requires native orders and info tables");
    }
    void data(const cg::Plaza2ListenerEvent& e) {
        require(e.raw_table && transaction_, "probe row outside bound transaction");
        const auto name = e.raw_table->name;
        if (name == "info") {
            require(e.table_index == info_index_, "probe info index mismatch");
            const auto publication = read<std::int8_t>(e, publication_);
            const auto rev = read<std::int64_t>(e, info_rev_), life = read<std::int64_t>(e, info_life_);
            require((publication == 0 || publication == 1) && rev >= 0 && life >= 0, "invalid probe snapshot info");
            info_done_ = publication == 1;
            if (info_done_) {
                pending_.first = static_cast<std::uint64_t>(life);
                pending_.second = rev;
                have_info_ = true;
            }
            return;
        }
        if (book_) {
            if (name == "orders_log" || name == "multileg_orders_log" || name == "heartbeat" || name == "sys_events") {
                // Table-local replRev is not a documented global linked watermark.
                // A later log-mutated state cannot reuse the native snapshot anchor.
                log_rows_seen_ = true;
            }
            return;
        }
        if (name != "orders")
            return;
        require(e.table_index == order_index_, "reference orders index mismatch");
        const auto row = read<std::int64_t>(e, fields_.row), rev = read<std::int64_t>(e, fields_.rev);
        require(row > 0 && rev >= 0, "invalid reference row identity");
        const auto previous = orders_.find(row);
        if (previous != orders_.end() && previous->second.revision > rev)
            return;
        orders_.erase(row);
        if (read<std::int64_t>(e, fields_.act))
            return;
        const auto isin = read<std::int32_t>(e, fields_.isin);
        if (!std::binary_search(ids_.begin(), ids_.end(), isin))
            return;
        const auto status = static_cast<std::uint64_t>(read<std::int64_t>(e, fields_.status));
        if (status & (0x4ULL | 0x80002ULL | 0x8000000ULL))
            return;
        const auto qty = read<std::int64_t>(e, fields_.rest);
        const auto dir = read<std::int8_t>(e, fields_.dir);
        const auto price =
            moex::plaza2::public_wire::decimal_scaled(read<moex::plaza2::public_wire::Bcd16_5>(e, fields_.price));
        require(price && qty >= 0 && (dir == 1 || dir == 2), "invalid reference quote");
        if (!qty)
            return;
        require(orders_.size() < order_capacity_, "bounded reference order capacity exhausted");
        orders_.emplace(row, Order{isin, dir == 1, *price, qty, rev});
    }

  public:
    std::uint64_t p9_events{}, p9_erased{}, clear_events{}, unanchored_commits{};
    Observer(Window& window, std::span<const std::int32_t> ids, cg::Plaza2FullOrderLog* book = nullptr,
             std::size_t capacity = 200000, std::ostream* output = nullptr, std::uint64_t diagnostic_limit = 512)
        : window_(window), book_(book), ids_(ids.begin(), ids.end()), order_capacity_(capacity), output_(output),
          diagnostic_limit_(diagnostic_limit) {
        std::sort(ids_.begin(), ids_.end());
        require(!ids_.empty() && capacity > 0, "invalid probe universe or capacity");
        orders_.reserve(capacity);
    }
    bool wants_raw_replication(std::string_view) const noexcept override {
        return true;
    }
    bool should_log_listener_event(const cg::Plaza2ListenerEvent&) const noexcept override {
        return false;
    }
    bool needs_fresh_snapshot() const noexcept override {
        return book_ && book_->needs_fresh_snapshot();
    }
    cg::Plaza2Error on_plaza2_listener_event(const cg::Plaza2ListenerEvent& e) override {
        try {
            using Kind = cg::Plaza2ListenerEventKind;
            const unsigned side = book_ ? 0 : 1;
            if (e.kind == Kind::Open) {
                bind(e.raw_tables);
                window_.retire(side);
                orders_.clear();
                committed_ = {};
                pending_ = {};
                transaction_ = info_done_ = have_info_ = log_rows_seen_ = false;
            }
            if (e.kind == Kind::TransactionBegin) {
                require(!transaction_, "nested probe transaction");
                transaction_ = true;
                pending_ = committed_;
                info_done_ = false;
            }
            if (e.kind == Kind::StreamData)
                data(e);
            std::uint64_t erased = 0;
            const auto prior = book_ ? book_->metrics().clear_deleted_erased : 0;
            const auto epoch = book_ ? book_->epoch() : 0;
            if (book_) {
                const auto error = book_->on_plaza2_listener_event(e);
                if (error)
                    return error;
                erased = book_->metrics().clear_deleted_erased - prior;
            }
            if (e.kind == Kind::ClearDeleted) {
                ++clear_events;
                const auto name = e.raw_table ? e.raw_table->name : std::string("unresolved");
                if (!book_ && name == "orders") {
                    for (auto i = orders_.begin(); i != orders_.end();) {
                        if (e.signed_value == INT64_MAX || i->second.revision < e.signed_value) {
                            i = orders_.erase(i);
                            ++erased;
                        } else
                            ++i;
                    }
                    if (e.signed_value == INT64_MAX) {
                        have_info_ = false;
                        window_.retire(side);
                    }
                }
                if (book_ && name == "orders_log") {
                    ++p9_events;
                    p9_erased += erased;
                }
                if (output_ && diagnostics_++ < diagnostic_limit_)
                    *output_ << "{\"kind\":\"clear_deleted\",\"source\":\"" << (book_ ? "composite" : "reference")
                             << "\",\"table\":" << std::quoted(name) << ",\"table_index\":" << e.table_index
                             << ",\"cutoff_revision\":" << e.signed_value << ",\"flags\":" << e.clear_deleted_flags
                             << ",\"erased\":" << erased << "}\n"
                             << std::flush;
            }
            if (book_ && epoch != book_->epoch()) {
                window_.retire(side);
                have_info_ = false;
                committed_ = {};
                pending_ = {};
            }
            if (e.kind == Kind::TransactionCommit) {
                require(transaction_, "probe commit without begin");
                transaction_ = false;
                committed_ = pending_;
                if (book_ && log_rows_seen_)
                    ++unanchored_commits;
                if (have_info_ && info_done_ && (!book_ || !log_rows_seen_))
                    window_.record(side, committed_, digest(levels(), ids_));
            }
            if (e.kind == Kind::Close || (!book_ && e.kind == Kind::LifeNum)) {
                orders_.clear();
                have_info_ = false;
                window_.retire(side);
            }
            return {};
        } catch (const std::exception& error) {
            return {.code = cg::Plaza2ErrorCode::DecodeFailed, .message = error.what()};
        }
    }
};
} // namespace full_order_log_probe
