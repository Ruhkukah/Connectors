#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "moex/plaza2/cgate/plaza2_decimal.hpp"
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace moex::plaza2::cgate {
namespace {
// Fixed nodes are reused on erase; large_ holds both construction-time hash bucket arrays.
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
    BookArena(std::size_t orders, std::size_t levels)
        : nodes_(orders * 2 + levels * 4 + 32), large_(orders * 48 + 4096) {}
    std::size_t bytes() const {
        return nodes_.size() * sizeof(Block) + large_.size();
    }
};
constexpr std::uint64_t kNonQuote = 0x4, kIoc = 0x80002, kMultileg = 0x8000000;
Plaza2Error failure(std::string_view message) {
    return {.code = Plaza2ErrorCode::DecodeFailed, .message = std::string(message)};
}
using BoundField = Plaza2RawFieldBinding;
void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::invalid_argument("Full Order Log " + std::string(message));
}
struct Binding {
    std::size_t index{}, row_size{};
    BoundField isin, order, row, rev, act, price, rest, status, dir, action, moment;
};
BoundField field(const Plaza2RawTableBinding& table, std::string_view name, std::string_view type, std::size_t size) {
    const auto i =
        std::find_if(table.fields.begin(), table.fields.end(), [&](const auto& f) { return f.name == name; });
    if (i == table.fields.end() || i->type_token != type || i->size != size || i->offset > table.row_size ||
        size > table.row_size - i->offset)
        throw std::invalid_argument("Full Order Log missing or incompatible field: " + std::string(name));
    return *i;
}
Binding bind(const Plaza2RawTableBinding& t, bool log) {
    Binding b{.index = t.index, .row_size = t.row_size};
    b.isin = field(t, "isin_id", "i4", 4);
    b.order = field(t, "public_order_id", "i8", 8);
    b.row = field(t, "replID", "i8", 8);
    b.rev = field(t, "replRev", "i8", 8);
    b.act = field(t, "replAct", "i8", 8);
    b.price = field(t, "price", "d16.5", 11);
    b.rest = field(t, "public_amount_rest", "i8", 8);
    b.status = field(t, "xstatus", "i8", 8);
    b.dir = field(t, "dir", "i1", 1);
    b.moment = field(t, "moment_ns", "u8", 8);
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
        LevelMap published[2], dirty[2];
        std::uint64_t revision{}, moment{}, committed_moment{};
        bool touched{}, crossed{};
        explicit Instrument(std::pmr::memory_resource* r)
            : published{LevelMap(r), LevelMap(r)}, dirty{LevelMap(r), LevelMap(r)} {}
    };
    struct Source {
        std::int64_t row{}, revision{};
    };
    struct Order {
        std::int64_t price{}, quantity{};
        Source source[2]{};
        std::size_t instrument{};
        unsigned current{};
        bool bid{};
    };
    BookArena arena;
    std::pmr::unordered_map<std::int64_t, Order> orders;
    std::pmr::unordered_set<std::int64_t> excluded;
    std::vector<std::int32_t> isins;
    std::vector<Instrument> books;
    std::size_t order_capacity, level_capacity, active_levels{}, dirty_levels{}, committed_count{};
    Binding log, snapshot;
    BoundField info_life, info_publication;
    std::size_t info_index{};
    FullOrderLogMetrics metrics;
    std::uint64_t epoch{}, life{}, initial_life{}, info_log_life{}, pending_log_life{};
    bool have_life{}, have_info{}, pending_info{}, bound{}, online{}, transaction{}, valid{}, waiting_snapshot{},
        snapshot_seen{};
    Clock::time_point committed_at{};
    Impl(std::span<const std::int32_t> ids, std::size_t oc, std::size_t lc)
        : arena(oc, lc), orders(&arena), excluded(&arena), isins(ids.begin(), ids.end()), order_capacity(oc),
          level_capacity(lc) {
        require(oc && lc && !ids.empty(), "requires instruments and capacities");
        std::sort(isins.begin(), isins.end());
        require(std::adjacent_find(isins.begin(), isins.end()) == isins.end(), "duplicate instrument");
        orders.max_load_factor(0.7f);
        orders.reserve(oc);
        excluded.max_load_factor(0.7f);
        excluded.reserve(oc);
        books.reserve(isins.size());
        for (auto id : isins) {
            require(id > 0, "invalid isin");
            books.emplace_back(&arena);
        }
    }
    std::size_t instrument(std::int32_t id) const {
        const auto i = std::lower_bound(isins.begin(), isins.end(), id);
        return i == isins.end() || *i != id ? isins.size() : i - isins.begin();
    }
    void clear() {
        orders.clear();
        excluded.clear();
        active_levels = dirty_levels = committed_count = 0;
        for (auto& b : books) {
            for (auto side : {0, 1}) {
                b.published[side].clear();
                b.dirty[side].clear();
            }
            b.revision = b.moment = b.committed_moment = 0;
            b.touched = b.crossed = false;
        }
        valid = online = transaction = have_info = pending_info = snapshot_seen = false;
        info_log_life = pending_log_life = 0;
        committed_at = {};
        ++epoch;
    }
    void exclude(std::int64_t id) {
        ++metrics.excluded_adds;
        if (!excluded.contains(id) && excluded.size() == order_capacity) {
            excluded.erase(excluded.begin());
            ++metrics.excluded_evictions;
        }
        excluded.insert(id);
    }
    void adjust(const Order& o, std::int64_t delta) {
        auto& book = books[o.instrument];
        const auto side = o.bid ? 0 : 1;
        const auto& levels = book.dirty[side].contains(o.price) ? book.dirty[side] : book.published[side];
        const auto current = levels.find(o.price);
        const auto old = current == levels.end() ? 0 : current->second;
        require(delta >= -old && (delta <= 0 || old <= INT64_MAX - delta), "aggregate quantity overflow");
        const auto quantity = old + delta;
        if (quantity && !old) {
            if (active_levels == level_capacity)
                throw std::bad_alloc();
            ++active_levels;
        } else if (!quantity && old)
            --active_levels;
        if (book.dirty[side].insert_or_assign(o.price, quantity).second && ++dirty_levels > level_capacity * 2)
            throw std::bad_alloc();
    }
    void touch(std::size_t index, const Plaza2ListenerEvent& e, const Binding& b) {
        auto& book = books[index];
        book.moment = std::max(book.moment, read<std::uint64_t>(e, b.moment));
        book.touched = true;
    }
    void erase(std::pmr::unordered_map<std::int64_t, Order>::iterator i) {
        adjust(i->second, -i->second.quantity);
        orders.erase(i);
    }
    void row(const Plaza2ListenerEvent& e) {
        require(e.raw_table != nullptr, "missing row binding");
        if (e.raw_table->name == "info") {
            require(transaction && e.table_index == info_index, "invalid info delivery");
            const auto publication = read<std::int8_t>(e, info_publication);
            const auto value = read<std::int64_t>(e, info_life);
            require(publication >= 0 && publication <= 1 && value >= 0, "invalid snapshot info");
            if (publication == 1) {
                pending_log_life = value;
                pending_info = true;
                snapshot_seen = true;
            }
            return;
        }
        const bool is_log = e.raw_table->name == "orders_log";
        if (!is_log && e.raw_table->name != "orders")
            return;
        ++metrics.rows_total;
        const auto& b = is_log ? log : snapshot;
        require(bound && e.table_index == b.index, "unknown table index");
        if (!is_log)
            snapshot_seen = true;
        const auto index = instrument(read<std::int32_t>(e, b.isin));
        if (index == isins.size()) {
            ++metrics.rows_filtered;
            return;
        }
        require(transaction && e.raw_payload.size() >= b.row_size, "invalid row transaction or size");
        const auto id = read<std::int64_t>(e, b.order), rev = read<std::int64_t>(e, b.rev),
                   row_id = read<std::int64_t>(e, b.row);
        const auto act = read<std::int64_t>(e, b.act);
        const unsigned source = is_log ? 1 : 0;
        auto i = orders.find(id);
        require(id > 0 && rev >= 0, "invalid identity or revision");
        require(i == orders.end() || i->second.instrument == index, "order identity contradicts instrument");
        if (act) {
            if (i != orders.end() && i->second.current == source && i->second.source[source].row == row_id &&
                rev >= i->second.source[source].revision) {
                touch(index, e, b);
                erase(i);
            } else
                ++metrics.retired_rows_ignored;
            return;
        }
        if (i != orders.end() && rev < i->second.source[source].revision) {
            ++metrics.retired_rows_ignored;
            return;
        }
        const auto action = is_log ? read<std::int8_t>(e, b.action) : 1;
        require(action >= 0 && action <= 2, "unknown action");
        if (action == 0) {
            if (i != orders.end()) {
                touch(index, e, b);
                erase(i);
            }
            excluded.erase(id);
            return;
        }
        if (i == orders.end() && action == 2) {
            ++metrics.ignored_executions;
            if (excluded.contains(id))
                ++metrics.excluded_executions;
            return;
        }
        const auto status = static_cast<std::uint64_t>(read<std::int64_t>(e, b.status));
        if (i == orders.end() && (status & (kNonQuote | kMultileg | kIoc))) {
            exclude(id);
            return;
        }
        const auto quantity = read<std::int64_t>(e, b.rest);
        const auto dir = read<std::int8_t>(e, b.dir);
        const auto price = public_wire::decimal_scaled(read<public_wire::Bcd16_5>(e, b.price));
        require(price && quantity >= 0 && (dir == 1 || dir == 2), "invalid price, remainder, or direction");
        touch(index, e, b);
        if (i != orders.end()) {
            auto& o = i->second;
            require(o.price == *price && o.bid == (dir == 1), "order attributes changed");
            require(action != 2 || quantity <= o.quantity, "execution increases remainder");
            if (!quantity) {
                erase(i);
                return;
            }
            adjust(o, quantity - o.quantity);
            o.quantity = quantity;
            o.source[source] = {row_id, rev};
            o.current = source;
        } else if (quantity) {
            if (orders.size() == order_capacity)
                throw std::bad_alloc();
            Order o{.price = *price, .quantity = quantity, .instrument = index, .current = source, .bid = dir == 1};
            o.source[source] = {row_id, rev};
            adjust(o, quantity);
            orders.emplace(id, o);
            excluded.erase(id);
        }
    }
    void commit(Plaza2FullOrderLog& owner) {
        if (online && pending_info && have_info && pending_log_life != info_log_life)
            throw std::invalid_argument("Full Order Log snapshot log life changed; fresh snapshot required");
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
                    if (!quantity)
                        levels.erase(i);
                    else
                        levels.insert_or_assign(price, quantity);
                }
                b.dirty[side].clear();
            }
            if (changed || (online && !b.revision))
                ++b.revision;
            const bool cross = !b.published[0].empty() && !b.published[1].empty() &&
                               b.published[0].rbegin()->first >= b.published[1].begin()->first;
            if (cross != b.crossed) {
                b.crossed = cross;
                ++metrics.crossed_transitions;
                if (owner.on_crossed)
                    owner.on_crossed(isins[n], cross);
            }
            b.committed_moment = b.moment;
        }
        if (pending_info) {
            info_log_life = pending_log_life;
            have_info = true;
            pending_info = false;
        }
        dirty_levels = 0;
        committed_count = orders.size();
        transaction = false;
        committed_at = Clock::now();
        valid = online && !waiting_snapshot;
        const auto utc =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        for (auto& b : books) {
            if (online && b.touched && b.moment) {
                const auto now = static_cast<std::uint64_t>(utc);
                if (b.moment > now)
                    ++metrics.exchange_ahead_of_clock;
                const auto lag = b.moment > now ? 0 : now - b.moment;
                ++metrics.lag_samples;
                metrics.lag_last_ns = lag;
                metrics.lag_max_ns = std::max(metrics.lag_max_ns, lag);
                metrics.lag_sum_ns += std::min(lag, UINT64_MAX - metrics.lag_sum_ns);
            }
            b.touched = false;
        }
    }
};

Plaza2FullOrderLog::Plaza2FullOrderLog(std::span<const std::int32_t> ids, std::size_t orders, std::size_t levels)
    : impl_(std::make_unique<Impl>(ids, orders, levels)) {}
Plaza2FullOrderLog::~Plaza2FullOrderLog() = default;
void Plaza2FullOrderLog::reset() {
    impl_->clear();
    impl_->waiting_snapshot = false;
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
            p.have_life = false;
            p.life = p.initial_life = 0;
            const auto table = [&](std::string_view name) -> const Plaza2RawTableBinding& {
                const auto i = std::find_if(e.raw_tables.begin(), e.raw_tables.end(),
                                            [&](const auto& t) { return t.name == name; });
                require(i != e.raw_tables.end(), "requires orders_log, orders and info");
                return *i;
            };
            p.log = bind(table("orders_log"), true);
            p.snapshot = bind(table("orders"), false);
            const auto& info = table("info");
            field(info, "trades_rev", "i8", 8);
            p.info_life = field(info, "trades_lifenum", "i8", 8);
            p.info_publication = field(info, "publication_state", "i1", 1);
            p.info_index = info.index;
            p.bound = true;
            break;
        }
        case Kind::TransactionBegin:
            require(!p.transaction && p.bound, "invalid transaction begin");
            p.transaction = true;
            break;
        case Kind::StreamData:
            p.row(e);
            break;
        case Kind::TransactionCommit:
            require(p.transaction, "commit without begin");
            p.commit(*this);
            if (p.valid && on_commit)
                on_commit(*this);
            break;
        case Kind::Online:
            if (!p.waiting_snapshot || p.snapshot_seen) {
                p.online = true;
                p.waiting_snapshot = false;
            }
            break; // visibility waits for the following commit
        case Kind::LifeNum:
            ++p.metrics.life_events;
            if (p.have_life && p.life != e.unsigned_value &&
                !(p.have_info && (p.info_log_life == e.unsigned_value || p.initial_life == e.unsigned_value))) {
                reset();
                p.waiting_snapshot = true;
            }
            if (!p.have_life)
                p.initial_life = e.unsigned_value;
            p.life = e.unsigned_value;
            p.have_life = true;
            break;
        case Kind::ClearDeleted: {
            if (!e.raw_table || (e.raw_table->name != "orders_log" && e.raw_table->name != "orders"))
                break;
            require(e.raw_table->index == e.table_index, "ClearDeleted index mismatch");
            if (e.signed_value == INT64_MAX) {
                const bool transaction = p.transaction;
                reset();
                p.waiting_snapshot = true;
                p.transaction = transaction;
                break;
            }
            const unsigned source = e.raw_table->name == "orders_log" ? 1 : 0;
            for (auto i = p.orders.begin(); i != p.orders.end();) {
                const auto& row = i->second.source[source];
                if (row.row && row.revision < e.signed_value) {
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
bool Plaza2FullOrderLog::crossed(std::int32_t isin) const noexcept {
    const auto n = impl_->instrument(isin);
    return n < impl_->books.size() && impl_->books[n].crossed;
}
std::uint64_t Plaza2FullOrderLog::exchange_moment_ns(std::int32_t isin) const noexcept {
    const auto n = impl_->instrument(isin);
    return n < impl_->books.size() ? impl_->books[n].committed_moment : 0;
}
FullOrderLogMetrics Plaza2FullOrderLog::metrics() const noexcept {
    auto result = impl_->metrics;
    result.excluded_ids = impl_->excluded.size();
    return result;
}
FullOrderLogLifeState Plaza2FullOrderLog::life_state() const noexcept {
    return {impl_->life, impl_->info_log_life, impl_->have_info};
}
bool Plaza2FullOrderLog::should_log_listener_event(const Plaza2ListenerEvent& e) const noexcept {
    return e.kind != Plaza2ListenerEventKind::StreamData || (e.raw_table && e.raw_table->name == "sys_events");
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
std::size_t Plaza2FullOrderLog::memory_bytes() const noexcept {
    return impl_->arena.bytes() + impl_->books.capacity() * sizeof(Impl::Instrument) +
           impl_->isins.capacity() * sizeof(std::int32_t) + sizeof(Impl);
}
std::size_t Plaza2FullOrderLog::order_count() const noexcept {
    return impl_->committed_count;
}
} // namespace moex::plaza2::cgate
