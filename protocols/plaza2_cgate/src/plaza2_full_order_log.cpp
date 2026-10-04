#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "moex/plaza2/cgate/plaza2_decimal.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace moex::plaza2::cgate {
namespace {
// Fixed node blocks are returned on erase and reused on the next insertion.
// Only the unordered-map's constructor-time bucket allocation uses large_.
class BookArena final : public std::pmr::memory_resource {
    struct alignas(std::max_align_t) Block {
        std::byte bytes[128];
    };
    std::vector<Block> nodes_;
    std::vector<std::byte> large_;
    std::size_t next_{}, large_next_{};
    void* free_{};
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (alignment > alignof(Block))
            throw std::bad_alloc();
        if (bytes <= sizeof(Block)) {
            if (free_) {
                void* p = free_;
                std::memcpy(&free_, p, sizeof(free_));
                return p;
            }
            if (next_ == nodes_.size())
                throw std::bad_alloc();
            return &nodes_[next_++];
        }
        const auto at = (large_next_ + alignment - 1) & ~(alignment - 1);
        if (at > large_.size() || bytes > large_.size() - at)
            throw std::bad_alloc();
        large_next_ = at + bytes;
        return large_.data() + at;
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t) override {
        if (bytes <= sizeof(Block)) {
            std::memcpy(p, &free_, sizeof(free_));
            free_ = p;
        }
    }
    bool do_is_equal(const std::pmr::memory_resource& rhs) const noexcept override {
        return this == &rhs;
    }

  public:
    BookArena(std::size_t orders, std::size_t levels) : nodes_(orders + levels * 5 + 32), large_(orders * 24 + 4096) {}
    std::size_t bytes() const {
        return nodes_.size() * sizeof(Block) + large_.size();
    }
};
constexpr std::uint64_t kNonQuote = 0x4, kIoc = 0x80002, kMultileg = 0x8000000;
Plaza2Error failure(std::string_view message) {
    return {.code = Plaza2ErrorCode::DecodeFailed, .message = std::string(message)};
}
struct BoundField {
    std::size_t offset{}, size{}, index{};
};
struct Binding {
    std::size_t index{}, row_size{};
    BoundField isin, order, rev, act, price, rest, status, dir, action;
};
BoundField field(const Plaza2RawTableBinding& table, std::string_view name, std::string_view type, std::size_t size) {
    const auto i =
        std::find_if(table.fields.begin(), table.fields.end(), [&](const auto& f) { return f.name == name; });
    if (i == table.fields.end() || i->type_token != type || i->size != size || i->offset > table.row_size ||
        size > table.row_size - i->offset)
        throw std::invalid_argument("Full Order Log missing or incompatible field: " + std::string(name));
    return {i->offset, i->size, i->index};
}
Binding bind(const Plaza2RawTableBinding& t, bool log) {
    Binding b{.index = t.index, .row_size = t.row_size};
    b.isin = field(t, "isin_id", "i4", 4);
    b.order = field(t, "public_order_id", "i8", 8);
    b.rev = field(t, "replRev", "i8", 8);
    b.act = field(t, "replAct", "i8", 8);
    b.price = field(t, "price", "d16.5", 11);
    b.rest = field(t, "public_amount_rest", "i8", 8);
    b.status = field(t, "xstatus", "i8", 8);
    b.dir = field(t, "dir", "i1", 1);
    if (log)
        b.action = field(t, "public_action", "i1", 1);
    return b;
}
template <class T> T read(const Plaza2ListenerEvent& e, const BoundField& f) {
    if (f.offset > e.raw_payload.size() || f.size > e.raw_payload.size() - f.offset ||
        (!e.raw_nulls.empty() && (f.index >= e.raw_nulls.size() || e.raw_nulls[f.index])))
        throw std::invalid_argument("Full Order Log truncated or null required field");
    return public_wire::load<T>(e.raw_payload, f.offset);
}
} // namespace

struct Plaza2FullOrderLog::Impl {
    struct Instrument {
        LevelMap working[2], published[2], dirty[2];
        std::uint64_t revision{};
        explicit Instrument(std::pmr::memory_resource* r)
            : working{LevelMap(r), LevelMap(r)}, published{LevelMap(r), LevelMap(r)}, dirty{LevelMap(r), LevelMap(r)} {}
    };
    struct Order {
        std::int64_t price{}, quantity{}, revision{};
        std::size_t instrument{}, table{};
        bool bid{};
    };
    BookArena arena;
    std::pmr::unordered_map<std::int64_t, Order> orders;
    std::vector<std::int32_t> isins;
    std::vector<Instrument> books;
    std::vector<FullOrderLogLevelChange> changes;
    std::size_t order_capacity, level_capacity, active_levels{}, dirty_levels{}, committed_count{};
    Binding log, snapshot;
    std::uint64_t epoch{}, life{};
    bool have_life{}, bound{}, online{}, transaction{}, valid{};
    Clock::time_point committed_at{};
    Impl(std::span<const std::int32_t> ids, std::size_t oc, std::size_t lc)
        : arena(oc, lc), orders(&arena), isins(ids.begin(), ids.end()), order_capacity(oc), level_capacity(lc) {
        if (!oc || !lc || ids.empty())
            throw std::invalid_argument("Full Order Log requires instruments and capacities");
        std::sort(isins.begin(), isins.end());
        if (std::adjacent_find(isins.begin(), isins.end()) != isins.end())
            throw std::invalid_argument("Full Order Log duplicate instrument");
        orders.max_load_factor(0.7f);
        orders.reserve(oc);
        books.reserve(isins.size());
        for (auto id : isins) {
            if (id <= 0)
                throw std::invalid_argument("invalid Full Order Log isin");
            books.emplace_back(&arena);
        }
        changes.reserve(lc * 2);
    }
    std::size_t instrument(std::int32_t id) const {
        const auto i = std::lower_bound(isins.begin(), isins.end(), id);
        return i == isins.end() || *i != id ? isins.size() : i - isins.begin();
    }
    void clear() {
        orders.clear();
        changes.clear();
        active_levels = dirty_levels = committed_count = 0;
        for (auto& b : books) {
            for (auto side : {0, 1}) {
                b.working[side].clear();
                b.published[side].clear();
                b.dirty[side].clear();
            }
            b.revision = 0;
        }
        valid = online = transaction = false;
        committed_at = {};
        ++epoch;
    }
    void adjust(const Order& o, std::int64_t delta) {
        auto& book = books[o.instrument];
        const auto side = o.bid ? 0 : 1;
        auto& levels = book.working[side];
        auto i = levels.find(o.price);
        const auto old = i == levels.end() ? 0 : i->second;
        if ((delta > 0 && old > std::numeric_limits<std::int64_t>::max() - delta) || delta < -old)
            throw std::invalid_argument("Full Order Log aggregate quantity overflow");
        const auto quantity = old + delta;
        if (quantity && i == levels.end()) {
            if (active_levels == level_capacity)
                throw std::bad_alloc();
            levels.emplace(o.price, quantity);
            ++active_levels;
        } else if (quantity)
            i->second = quantity;
        else if (i != levels.end()) {
            levels.erase(i);
            --active_levels;
        }
        auto& dirty = book.dirty[side];
        auto d = dirty.find(o.price);
        if (d == dirty.end()) {
            if (dirty_levels == level_capacity * 2)
                throw std::bad_alloc();
            dirty.emplace(o.price, quantity);
            ++dirty_levels;
        } else
            d->second = quantity;
    }
    void erase(std::pmr::unordered_map<std::int64_t, Order>::iterator i) {
        adjust(i->second, -i->second.quantity);
        orders.erase(i);
    }
    void row(const Plaza2ListenerEvent& e) {
        if (!e.raw_table)
            throw std::invalid_argument("Full Order Log missing row binding");
        const bool is_log = e.raw_table->name == "orders_log";
        if (!is_log && e.raw_table->name != "orders")
            return; // includes multileg tables
        const auto& b = is_log ? log : snapshot;
        if (!bound || e.table_index != b.index)
            throw std::invalid_argument("Full Order Log unknown table index");
        const auto index = instrument(read<std::int32_t>(e, b.isin));
        if (index == isins.size())
            return; // no other field decoded for excluded ISINs
        if (!transaction)
            throw std::invalid_argument("Full Order Log row outside transaction");
        if (e.raw_payload.size() < b.row_size)
            throw std::invalid_argument("Full Order Log truncated row");
        const auto id = read<std::int64_t>(e, b.order), rev = read<std::int64_t>(e, b.rev);
        const auto act = read<std::int64_t>(e, b.act);
        auto i = orders.find(id);
        if (id <= 0 || rev < 0)
            throw std::invalid_argument("Full Order Log invalid identity or revision");
        if (i != orders.end() && i->second.instrument != index)
            throw std::invalid_argument("Full Order Log order identity contradicts instrument");
        if (act) {
            if (i != orders.end())
                erase(i);
            return;
        }
        const auto status = static_cast<std::uint64_t>(read<std::int64_t>(e, b.status));
        if (status & (kNonQuote | kMultileg))
            return;
        const auto action = is_log ? read<std::int8_t>(e, b.action) : 1;
        if (action < 0 || action > 2)
            throw std::invalid_argument("Full Order Log unknown action");
        if (action == 0) {
            if (i != orders.end())
                erase(i);
            return;
        }
        // IOC orders never leave a queued remainder. Execution rows for a
        // resting order still update it even if the operation carries IOC.
        if (action == 1 && (status & kIoc))
            return;
        const auto quantity = read<std::int64_t>(e, b.rest);
        const auto dir = read<std::int8_t>(e, b.dir);
        const auto price = public_wire::decimal_scaled(read<public_wire::Bcd16_5>(e, b.price));
        if (!price || quantity < 0 || (dir != 1 && dir != 2))
            throw std::invalid_argument("Full Order Log invalid price, remainder, or direction");
        if (i != orders.end()) {
            auto& o = i->second;
            if (o.price != *price || o.bid != (dir == 1))
                throw std::invalid_argument("Full Order Log order attributes changed");
            if (action == 2 && quantity > o.quantity)
                throw std::invalid_argument("Full Order Log execution increases remainder");
            if (!quantity) {
                erase(i);
                return;
            }
            adjust(o, quantity - o.quantity);
            o.quantity = quantity;
            o.revision = rev;
            o.table = b.index;
        } else if (quantity && action == 1) {
            if (orders.size() == order_capacity)
                throw std::bad_alloc();
            Order o{*price, quantity, rev, index, b.index, dir == 1};
            adjust(o, quantity);
            orders.emplace(id, o);
        } else if (quantity && !(status & kIoc))
            throw std::invalid_argument("Full Order Log execution references missing order");
    }
    void commit() {
        changes.clear();
        for (std::size_t n = 0; n < books.size(); ++n) {
            auto& b = books[n];
            bool changed = false;
            for (auto side : {0, 1}) {
                for (const auto& [price, quantity] : b.dirty[side]) {
                    auto& levels = b.published[side];
                    auto i = levels.find(price);
                    const auto old = i == levels.end() ? 0 : i->second;
                    if (old == quantity)
                        continue;
                    changed = true;
                    changes.push_back({isins[n], side == 0, price, quantity});
                    if (!quantity) {
                        if (i != levels.end())
                            levels.erase(i);
                    } else if (i == levels.end())
                        levels.emplace(price, quantity);
                    else
                        i->second = quantity;
                }
                b.dirty[side].clear();
            }
            if (changed)
                ++b.revision;
        }
        dirty_levels = 0;
        committed_count = orders.size();
        transaction = false;
        committed_at = Clock::now();
        valid = online;
    }
};

Plaza2FullOrderLog::Plaza2FullOrderLog(std::span<const std::int32_t> ids, std::size_t orders, std::size_t levels)
    : impl_(std::make_unique<Impl>(ids, orders, levels)) {}
Plaza2FullOrderLog::~Plaza2FullOrderLog() = default;
void Plaza2FullOrderLog::reset() {
    impl_->clear();
    if (on_invalidate)
        on_invalidate();
}
void Plaza2FullOrderLog::on_plaza2_listener_error(const Plaza2Error&) noexcept {
    try {
        reset();
    } catch (...) {
    }
}
Plaza2Error Plaza2FullOrderLog::on_plaza2_listener_event(const Plaza2ListenerEvent& e) {
    auto& p = *impl_;
    try {
        using Kind = Plaza2ListenerEventKind;
        switch (e.kind) {
        case Kind::Open: {
            reset();
            p.bound = false;
            bool log = false, snapshot = false, info = false;
            for (const auto& t : e.raw_tables) {
                if (t.name == "orders_log") {
                    p.log = bind(t, true);
                    log = true;
                } else if (t.name == "orders") {
                    p.snapshot = bind(t, false);
                    snapshot = true;
                } else if (t.name == "info") {
                    field(t, "trades_rev", "i8", 8);
                    field(t, "trades_lifenum", "i8", 8);
                    field(t, "publication_state", "i1", 1);
                    info = true;
                }
            }
            if (!log || !snapshot || !info)
                throw std::invalid_argument("Full Order Log requires orders_log, orders and info");
            p.bound = true;
            break;
        }
        case Kind::TransactionBegin:
            if (p.transaction || !p.bound)
                throw std::invalid_argument("Full Order Log invalid transaction begin");
            p.transaction = true;
            break;
        case Kind::StreamData:
            p.row(e);
            break;
        case Kind::TransactionCommit:
            if (!p.transaction)
                throw std::invalid_argument("Full Order Log commit without begin");
            p.commit();
            if (p.valid && on_commit)
                on_commit(*this);
            break;
        case Kind::Online:
            p.online = true;
            break; // visibility waits for the following commit
        case Kind::LifeNum:
            if (p.have_life && p.life != e.unsigned_value)
                reset();
            p.life = e.unsigned_value;
            p.have_life = true;
            break;
        case Kind::ClearDeleted: {
            if (!e.raw_table || (e.raw_table->name != "orders_log" && e.raw_table->name != "orders"))
                break;
            if (e.raw_table->index != e.table_index)
                throw std::invalid_argument("Full Order Log ClearDeleted index mismatch");
            for (auto i = p.orders.begin(); i != p.orders.end();) {
                if (i->second.table == e.table_index && (e.signed_value == std::numeric_limits<std::int64_t>::max() ||
                                                         i->second.revision < e.signed_value)) {
                    auto old = i++;
                    p.erase(old);
                } else
                    ++i;
            }
            break;
        }
        case Kind::Close:
            reset();
            p.bound = false;
            break;
        default:
            break;
        }
    } catch (const std::bad_alloc&) {
        reset();
        return failure("Full Order Log bounded storage exhausted; fresh snapshot required");
    } catch (const std::exception& ex) {
        auto error = failure(ex.what());
        if (e.kind == Plaza2ListenerEventKind::Open)
            error.code = Plaza2ErrorCode::IncompatibleScheme;
        reset();
        return error;
    }
    return {};
}
bool Plaza2FullOrderLog::valid() const noexcept {
    return impl_->valid;
}
std::uint64_t Plaza2FullOrderLog::epoch() const noexcept {
    return impl_->epoch;
}
std::uint64_t Plaza2FullOrderLog::revision(std::int32_t isin) const noexcept {
    const auto n = impl_->instrument(isin);
    return n == impl_->books.size() ? 0 : impl_->books[n].revision;
}
Plaza2FullOrderLog::Clock::time_point Plaza2FullOrderLog::committed_at() const noexcept {
    return impl_->committed_at;
}
std::span<const std::int32_t> Plaza2FullOrderLog::instruments() const noexcept {
    return impl_->isins;
}
const Plaza2FullOrderLog::LevelMap& Plaza2FullOrderLog::levels(std::int32_t isin, bool bid) const {
    const auto n = impl_->instrument(isin);
    if (n == impl_->books.size())
        throw std::out_of_range("Full Order Log instrument not configured");
    return impl_->books[n].published[bid ? 0 : 1];
}
std::span<const FullOrderLogLevelChange> Plaza2FullOrderLog::changes() const noexcept {
    return impl_->changes;
}
std::size_t Plaza2FullOrderLog::memory_bytes() const noexcept {
    return impl_->arena.bytes() + impl_->changes.capacity() * sizeof(FullOrderLogLevelChange) +
           impl_->books.capacity() * sizeof(Impl::Instrument) + impl_->isins.capacity() * sizeof(std::int32_t) +
           sizeof(Impl);
}
std::size_t Plaza2FullOrderLog::order_count() const noexcept {
    return impl_->committed_count;
}
} // namespace moex::plaza2::cgate
