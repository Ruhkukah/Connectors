#include "moex/plaza2/cgate/plaza2_full_order_log_deals.hpp"
#include "moex/plaza2/cgate/plaza2_decimal.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <stdexcept>
#include <utility>

namespace moex::plaza2::cgate {
namespace {
using Kind = Plaza2ListenerEventKind;
using Field = generated::FieldCode;
using Value = Plaza2DecodedValueKind;
constexpr auto kDealsStream = generated::StreamCode::kFortsDealsRepl;
constexpr auto kDealTable = generated::TableCode::kFortsDealsReplDeal;

void check(bool valid, const char* message) {
    if (!valid)
        throw std::invalid_argument(message);
}

struct FieldSpec {
    std::string_view name;
    std::string_view type;
    std::size_t size;
    Field code;
};

constexpr std::array specs{
    FieldSpec{"isin_id", "i4", 4, Field::kFortsDealsReplDealIsinId},
    FieldSpec{"sess_id", "i4", 4, Field::kFortsDealsReplDealSessId},
    FieldSpec{"nosystem", "i1", 1, Field::kFortsDealsReplDealNosystem},
    FieldSpec{"replID", "i8", 8, Field::kFortsDealsReplDealReplId},
    FieldSpec{"replRev", "i8", 8, Field::kFortsDealsReplDealReplRev},
    FieldSpec{"replAct", "i8", 8, Field::kFortsDealsReplDealReplAct},
    FieldSpec{"id_deal", "i8", 8, Field::kFortsDealsReplDealIdDeal},
    FieldSpec{"xamount", "i8", 8, Field::kFortsDealsReplDealXamount},
    FieldSpec{"price", "d16.5", 11, Field::kFortsDealsReplDealPrice},
    FieldSpec{"moment", "t", 10, Field::kFortsDealsReplDealMoment},
    FieldSpec{"moment_ns", "u8", 8, Field::kFortsDealsReplDealMomentNs},
    FieldSpec{"xstatus_buy", "i8", 8, Field::kFortsDealsReplDealXstatusBuy},
    FieldSpec{"xstatus_sell", "i8", 8, Field::kFortsDealsReplDealXstatusSell},
};

std::uint64_t unix_seconds(public_wire::Time t) {
    using namespace std::chrono;
    const auto date = year{t.year} / month{t.month} / day{t.day};
    check(date.ok() && t.hour < 24 && t.minute < 60 && t.second < 60 && t.msec < 1000, "invalid DEALS moment");
    const auto time = sys_days{date} + hours{t.hour} + minutes{t.minute} + seconds{t.second} - hours{3};
    const auto count = time.time_since_epoch().count();
    check(count > 0, "DEALS moment is outside Unix time");
    return static_cast<std::uint64_t>(count);
}

Plaza2ListenerEvent control(Kind kind, std::uint64_t value = 0) {
    return {.kind = kind, .stream_code = kDealsStream, .unsigned_value = value};
}

void deliver(Plaza2PublicDealsBridge& bridge, const Plaza2ListenerEvent& event) {
    if (const auto error = bridge.on_plaza2_listener_event(event); error)
        throw std::invalid_argument(error.message);
}
} // namespace

struct FullOrderLogDeals::Impl {
    struct Target {
        std::unique_ptr<Plaza2PublicDealsBridge> bridge;
        std::int32_t session{};
        std::uint64_t epoch{}, cursor{};
        bool touched{};
    };
    struct Slot {
        Target current, next;
    };
    struct Binding {
        std::size_t offset{}, index{};
        bool present{};
    };

    std::map<std::int32_t, Slot> slots;
    std::array<Binding, specs.size()> fields{};
    std::size_t table{}, row_size{};
    std::uint64_t life{}, next_epoch{};
    bool bound{}, online{}, transaction{}, online_pending{};

    void clear() noexcept {
        for (auto& [isin, slot] : slots)
            slot = {};
        online = transaction = online_pending = false;
    }

    void reset() noexcept {
        clear();
        bound = false;
        life = 0;
    }

    void bind(std::span<const Plaza2RawTableBinding> tables) {
        check(tables.size() == 1 && tables[0].name == "deal", "trade publisher requires exactly deal table");
        const auto& t = tables[0];
        table = t.index;
        row_size = t.row_size;
        for (std::size_t i = 0; i < specs.size(); ++i) {
            const auto& spec = specs[i];
            const auto f = std::ranges::find(t.fields, spec.name, &Plaza2RawFieldBinding::name);
            if (f == t.fields.end() && i >= specs.size() - 2) {
                fields[i] = {};
                continue;
            }
            check(f != t.fields.end() && f->type_token == spec.type && f->size == spec.size && f->offset <= row_size &&
                      spec.size <= row_size - f->offset,
                  "DEALS trade field/type missing or incompatible");
            fields[i] = {f->offset, f->index, true};
        }
        bound = true;
    }

    std::span<const std::byte> field(const Plaza2ListenerEvent& event, std::size_t i) const {
        const auto& b = fields[i];
        if (!b.present)
            return {};
        if (!event.raw_nulls.empty()) {
            check(b.index < event.raw_nulls.size(), "DEALS null map is truncated");
            if (event.raw_nulls[b.index])
                return {};
        }
        return event.raw_payload.subspan(b.offset, specs[i].size);
    }

    template <typename T> T required(const Plaza2ListenerEvent& event, std::size_t i) const {
        const auto raw = field(event, i);
        check(raw.size() == sizeof(T), "null required DEALS trade field");
        return public_wire::load<T>(raw);
    }

    void begin() {
        check(!transaction, "nested DEALS trade transaction");
        transaction = true;
    }

    std::uint64_t new_epoch() {
        check(next_epoch != UINT64_MAX, "DEALS trade generation exhausted");
        return ++next_epoch;
    }

    void publish_online() {
        online = true;
        online_pending = false;
        for (auto& [isin, slot] : slots) {
            if (slot.current.bridge)
                deliver(*slot.current.bridge, control(Kind::Online));
            else
                slot.current.epoch = new_epoch();
        }
    }

    void commit(const std::function<void(const Plaza2PublicDeal&)>& on_trade) {
        check(transaction, "DEALS trade commit without begin");
        std::vector<Plaza2PublicDeal> published;
        for (auto& [isin, slot] : slots) {
            for (auto* target : {&slot.current, &slot.next}) {
                if (!target->touched)
                    continue;
                deliver(*target->bridge, control(Kind::TransactionCommit));
                target->touched = false;
                if (on_trade && online) {
                    auto delta = target->bridge->snapshot(target->cursor);
                    target->cursor = delta.last_sequence;
                    for (auto& trade : delta.trades) {
                        trade.stream_epoch = target->epoch;
                        published.push_back(std::move(trade));
                    }
                }
            }
            if (slot.next.bridge)
                slot.current = std::exchange(slot.next, {});
        }
        transaction = false;
        if (online_pending)
            publish_online();
        std::ranges::stable_sort(published, {}, &Plaza2PublicDeal::repl_rev);
        for (const auto& trade : published)
            on_trade(trade);
    }

    void row(const Plaza2ListenerEvent& e) {
        check(bound && e.raw_table && e.raw_table->name == "deal" && e.table_index == table &&
                  e.raw_payload.size() >= row_size,
              "invalid DEALS trade row/table size");
        check(!online || transaction, "DEALS live trade row outside transaction");
        const auto isin = required<std::int32_t>(e, 0);
        const auto it = slots.find(isin);
        if (it == slots.end())
            return;
        const auto session = required<std::int32_t>(e, 1);
        if (required<std::int8_t>(e, 2) != 0)
            return;
        check(session > 0, "invalid DEALS trade session");
        check(life != 0, "DEALS trade row arrived without LifeNum");
        auto& slot = it->second;
        if (session < slot.current.session)
            return;
        auto& target = online && session != slot.current.session ? slot.next : slot.current;
        if (!online && session != target.session)
            target = {};
        check(!target.bridge || target.session == session, "multiple DEALS session transitions in one transaction");
        if (!target.bridge) {
            target = {.bridge = std::make_unique<Plaza2PublicDealsBridge>(isin, session),
                      .session = session,
                      .epoch = slot.current.bridge || !slot.current.epoch ? new_epoch() : slot.current.epoch};
            deliver(*target.bridge, control(Kind::LifeNum, life));
            deliver(*target.bridge, control(Kind::Open));
            if (online)
                deliver(*target.bridge, control(Kind::Online));
        }
        // Snapshot rows only seed the bounded dedup history. They never
        // reach subscribers, so we can consume even large snapshot batches
        // without accumulating them in the live transaction buffer.
        if (!target.touched) {
            deliver(*target.bridge, control(Kind::TransactionBegin));
            target.touched = true;
        }

        std::array<Plaza2DecodedFieldValue, specs.size()> decoded{};
        for (std::size_t i = 0; i < specs.size(); ++i) {
            auto& value = decoded[i];
            const auto& spec = specs[i];
            value.field_code = spec.code;
            const auto raw = field(e, i);
            if (raw.empty()) {
                check(i >= specs.size() - 2, "null required DEALS trade field");
                value.kind = Value::UnsignedInteger;
                continue;
            }
            value.type_token = spec.type;
            value.raw_value = raw;
            if (spec.type == "u8" || i >= specs.size() - 2) {
                value.kind = Value::UnsignedInteger;
                value.unsigned_value = public_wire::load<std::uint64_t>(raw);
            } else if (spec.type == "t") {
                value.kind = Value::Timestamp;
                value.unsigned_value = unix_seconds(public_wire::load<public_wire::Time>(raw));
            } else if (spec.type == "d16.5") {
                const auto price = public_wire::decimal_value(public_wire::load<public_wire::Bcd16_5>(raw));
                check(price.has_value(), "invalid DEALS trade price");
                value.kind = Value::Decimal;
                value.decimal_exact = true;
                value.decimal_mantissa = price->mantissa;
                value.decimal_scale = price->scale;
            } else {
                value.kind = Value::SignedInteger;
                if (spec.size == 1)
                    value.signed_value = public_wire::load<std::int8_t>(raw);
                else if (spec.size == 4)
                    value.signed_value = public_wire::load<std::int32_t>(raw);
                else
                    value.signed_value = public_wire::load<std::int64_t>(raw);
            }
        }
        auto projected = e;
        projected.table_code = kDealTable;
        projected.fields = decoded;
        deliver(*target.bridge, projected);
        if (!online) {
            deliver(*target.bridge, control(Kind::TransactionCommit));
            target.touched = false;
        }
    }
};

FullOrderLogDeals::FullOrderLogDeals(std::span<const std::int32_t> isins) : impl_(std::make_unique<Impl>()) {
    check(!isins.empty() && isins.size() <= 64, "DEALS trades require selected instruments");
    for (const auto isin : isins)
        check(isin > 0 && impl_->slots.try_emplace(isin).second, "invalid/duplicate DEALS trade instrument");
}
FullOrderLogDeals::~FullOrderLogDeals() = default;

void FullOrderLogDeals::on_error(const Plaza2Error&) noexcept {
    impl_->reset();
}

Plaza2Error FullOrderLogDeals::on_event(const Plaza2ListenerEvent& event) {
    auto& p = *impl_;
    try {
        check(event.stream_code == kDealsStream, "trade publisher received a different stream");
        switch (event.kind) {
        case Kind::Open: {
            const auto pending_life = p.bound ? 0 : p.life;
            p.reset();
            p.life = pending_life;
            p.bind(event.raw_tables);
            break;
        }
        case Kind::LifeNum:
            check(event.unsigned_value != 0 && (!p.life || p.life == event.unsigned_value),
                  "DEALS trade LifeNum changed; fresh snapshot required");
            p.life = event.unsigned_value;
            break;
        case Kind::TransactionBegin:
            check(p.bound, "DEALS trade transaction before Open");
            p.begin();
            break;
        case Kind::StreamData: {
            const bool unframed_snapshot = !p.online && !p.transaction;
            if (unframed_snapshot)
                p.begin();
            p.row(event);
            if (unframed_snapshot)
                p.commit(on_trade);
            break;
        }
        case Kind::TransactionCommit:
            p.commit(on_trade);
            break;
        case Kind::Online:
            check(p.bound && p.life, "DEALS trade ONLINE requires Open and LifeNum");
            if (!p.online) {
                if (p.transaction)
                    p.online_pending = true;
                else
                    p.publish_online();
            }
            break;
        case Kind::ClearDeleted:
            if (event.signed_value == INT64_MAX)
                p.clear();
            break;
        case Kind::Close:
            p.reset();
            break;
        default:
            break;
        }
        return {};
    } catch (const std::exception& ex) {
        p.reset();
        return {.code = event.kind == Kind::Open ? Plaza2ErrorCode::IncompatibleScheme : Plaza2ErrorCode::DecodeFailed,
                .message = std::string("FullOrderLog DEALS trades: ") + ex.what()};
    }
}

Plaza2PublicDealsSnapshot FullOrderLogDeals::snapshot(std::int32_t isin, std::uint64_t after,
                                                      std::int32_t expected_session) const {
    const auto found = impl_->slots.find(isin);
    if (!impl_->online || found == impl_->slots.end())
        return {};
    const auto& target = found->second.current;
    if (!target.bridge)
        return {.online = true, .valid = true, .stream_epoch = target.epoch, .lifenum = impl_->life};
    if (expected_session > 0 && target.session != expected_session)
        return {};
    auto result = target.bridge->snapshot(after);
    result.stream_epoch = target.epoch;
    for (auto& trade : result.trades)
        trade.stream_epoch = target.epoch;
    return result;
}
} // namespace moex::plaza2::cgate
