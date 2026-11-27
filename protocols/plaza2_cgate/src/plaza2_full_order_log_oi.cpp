#include "moex/plaza2/cgate/plaza2_full_order_log_oi.hpp"
#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

namespace moex::plaza2::cgate {
namespace {
void check(bool ok, const char* message) {
    if (!ok)
        throw std::invalid_argument(message);
}
struct OiField {
    std::size_t offset{}, index{}, size{};
};
OiField bind(const Plaza2RawTableBinding& t, std::string_view name, std::string_view type, std::size_t size) {
    const auto f = std::find_if(t.fields.begin(), t.fields.end(), [&](const auto& v) { return v.name == name; });
    check(f != t.fields.end() && f->type_token == type && f->size == size && f->offset <= t.row_size &&
              size <= t.row_size - f->offset,
          "OI required field/type missing");
    return {f->offset, f->index, size};
}
template <class T> T read(const Plaza2ListenerEvent& e, const OiField& f) {
    check(f.offset <= e.raw_payload.size() && sizeof(T) <= e.raw_payload.size() - f.offset &&
              (e.raw_nulls.empty() || (f.index < e.raw_nulls.size() && !e.raw_nulls[f.index])),
          "OI null/truncated required field");
    T v;
    std::memcpy(&v, e.raw_payload.data() + f.offset, sizeof v);
    return v;
}
} // namespace
struct FullOrderLogOpenInterest::Impl {
    struct Slot {
        std::optional<FullOrderLogOpenInterestValue> staged, committed;
        bool touched{};
    };
    std::map<std::int32_t, Slot> slots;
    std::size_t table{}, row_size{};
    OiField isin, session, quantity, revision, row_id, action, deal, moment;
    bool bound{}, transaction{}, online{}, life_seen{}, published{};
    std::vector<FullOrderLogOpenInterestValue> pending;
    std::uint64_t life{};
    explicit Impl(std::span<const std::int32_t> ids) {
        check(!ids.empty() && ids.size() <= 64, "OI requires selected instruments");
        for (auto id : ids)
            check(id > 0 && slots.emplace(id, Slot{}).second, "OI invalid/duplicate instrument");
    }
    void clear() {
        for (auto& [id, s] : slots)
            s = {};
        transaction = online = published = false;
        pending.clear();
    }
    void publish(FullOrderLogOpenInterest& owner) {
        for (auto& [id, s] : slots)
            if (s.touched) {
                s.committed = s.staged;
                s.touched = false;
                if (!published && s.committed && owner.on_update)
                    owner.on_update(*s.committed);
            }
        if (published && owner.on_update)
            for (const auto& v : pending)
                owner.on_update(v);
        pending.clear();
        published = true;
    }
};
FullOrderLogOpenInterest::FullOrderLogOpenInterest(std::span<const std::int32_t> ids)
    : impl_(std::make_unique<Impl>(ids)) {}
FullOrderLogOpenInterest::~FullOrderLogOpenInterest() = default;
bool FullOrderLogOpenInterest::online() const noexcept {
    return impl_->online;
}
std::optional<FullOrderLogOpenInterestValue> FullOrderLogOpenInterest::value(std::int32_t isin) const {
    const auto i = impl_->slots.find(isin);
    return impl_->online && i != impl_->slots.end() ? i->second.committed : std::nullopt;
}
bool FullOrderLogOpenInterest::should_log_listener_event(const Plaza2ListenerEvent& e) const noexcept {
    return e.kind == Plaza2ListenerEventKind::Open || e.kind == Plaza2ListenerEventKind::Close ||
           e.kind == Plaza2ListenerEventKind::Online || e.kind == Plaza2ListenerEventKind::LifeNum ||
           e.kind == Plaza2ListenerEventKind::ClearDeleted;
}
void FullOrderLogOpenInterest::on_plaza2_listener_error(const Plaza2Error& e) noexcept {
    try {
        if (before_error)
            before_error(e);
        impl_->clear();
        impl_->bound = false;
        if (on_invalidate)
            on_invalidate();
    } catch (...) {
    }
}
Plaza2Error FullOrderLogOpenInterest::on_plaza2_listener_event(const Plaza2ListenerEvent& e) {
    auto& p = *impl_;
    try {
        if (before_event)
            if (auto error = before_event(e); error) {
                p.clear();
                p.bound = false;
                return error;
            }
        check(e.stream_code == generated::StreamCode::kFortsDealsRepl, "OI received wrong stream");
        using K = Plaza2ListenerEventKind;
        const auto invalidate = [&] {
            p.clear();
            if (on_invalidate)
                on_invalidate();
        };
        switch (e.kind) {
        case K::Open: {
            invalidate();
            p.bound = false;
            p.life_seen = false;
            check(e.raw_tables.size() == 1 && e.raw_tables[0].name == "deal", "OI requires exactly deal table");
            const auto& t = e.raw_tables[0];
            p.table = t.index;
            p.row_size = t.row_size;
            p.isin = bind(t, "isin_id", "i4", 4);
            p.session = bind(t, "sess_id", "i4", 4);
            p.quantity = bind(t, "xpos", "i8", 8);
            p.revision = bind(t, "replRev", "i8", 8);
            p.row_id = bind(t, "replID", "i8", 8);
            p.action = bind(t, "replAct", "i8", 8);
            p.deal = bind(t, "id_deal", "i8", 8);
            p.moment = bind(t, "moment_ns", "u8", 8);
            p.bound = true;
            break;
        }
        case K::TransactionBegin:
            check(p.bound && !p.transaction, "OI invalid transaction begin");
            p.transaction = true;
            break;
        case K::TransactionCommit:
            check(p.transaction, "OI commit without begin");
            p.transaction = false;
            if (p.online)
                p.publish(*this);
            break;
        case K::StreamData: {
            check(p.bound && e.raw_table && e.raw_table->name == "deal" && e.table_index == p.table &&
                      e.raw_payload.size() >= p.row_size,
                  "OI invalid row/table size");
            check(!p.online || p.transaction, "OI row outside transaction");
            const auto isin = read<std::int32_t>(e, p.isin);
            const auto i = p.slots.find(isin);
            if (i == p.slots.end())
                break;
            auto& s = i->second;
            const auto rev = read<std::int64_t>(e, p.revision), id = read<std::int64_t>(e, p.row_id);
            check(rev >= 0 && id > 0, "OI invalid replication identity");
            if (s.staged && rev < s.staged->revision)
                break;
            if (read<std::int64_t>(e, p.action)) {
                if (s.staged && s.staged->row_id == id) {
                    s.staged.reset();
                    s.touched = true;
                }
                break;
            }
            FullOrderLogOpenInterestValue v{
                isin, read<std::int32_t>(e, p.session), read<std::int64_t>(e, p.quantity), rev,
                id,   read<std::int64_t>(e, p.deal),    read<std::uint64_t>(e, p.moment)};
            check(v.quantity >= 0 && v.session > 0 && v.deal_id > 0 && v.moment_ns > 0,
                  "OI invalid value/session/deal/time");
            s.staged = v;
            s.touched = true;
            if (p.online && p.published) {
                check(p.pending.size() < 262144, "OI transaction buffer exhausted");
                p.pending.push_back(v);
            }
            break;
        }
        case K::Online:
            check(p.bound, "OI ONLINE without schema");
            p.online = true;
            if (!p.transaction)
                p.publish(*this);
            break;
        case K::LifeNum:
            if (p.life_seen && p.life != e.unsigned_value) {
                invalidate();
                p.bound = false;
                return {.code = Plaza2ErrorCode::DecodeFailed,
                        .message = "FullOrderLog OI life changed; fresh snapshot required"};
            }
            p.life = e.unsigned_value;
            p.life_seen = true;
            break;
        case K::ClearDeleted:
            if (e.signed_value == INT64_MAX)
                invalidate();
            break; // Revision housekeeping does not undo a published observation.
        case K::Close:
            invalidate();
            p.bound = false;
            break;
        default:
            break;
        }
        return {};
    } catch (const std::exception& ex) {
        p.clear();
        p.bound = false;
        return {.code = e.kind == Plaza2ListenerEventKind::Open ? Plaza2ErrorCode::IncompatibleScheme
                                                                : Plaza2ErrorCode::DecodeFailed,
                .message = std::string("FullOrderLog OI: ") + ex.what()};
    }
}
} // namespace moex::plaza2::cgate
