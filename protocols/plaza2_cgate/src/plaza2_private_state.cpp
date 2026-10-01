#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "moex/plaza2/cgate/plaza2_fixed_point.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace moex::plaza2::private_state {

std::optional<SessionDecimal> parse_session_decimal(std::string_view text) noexcept {
    const auto units = cgate::parse_fixed_point(text, cgate::kPlaza2SessionFractionalDigits, true);
    return units.has_value() ? std::optional<SessionDecimal>{SessionDecimal{*units}} : std::nullopt;
}

FuturePriceBounds evaluate_future_price_bounds(const FutureSessionTerms& terms) noexcept {
    FuturePriceBounds result;
    if (!terms.settlement_price || !terms.raw_limit_up || !terms.raw_limit_down || !terms.min_step ||
        terms.min_step->units <= 0)
        return result;
    const auto reference = terms.settlement_price->units;
    const auto up = terms.raw_limit_up->units;
    const auto down = terms.raw_limit_down->units;
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if (!((up > 0 && reference > max - up) || (up < 0 && reference < min - up)))
        result.upper = SessionDecimal{reference + up};
    // Subtract directly: negating INT64_MIN would itself overflow.
    if (!((down > 0 && reference < min + down) || (down < 0 && reference > max + down)))
        result.lower = SessionDecimal{reference - down};
    result.interval_valid = result.lower && result.upper && result.lower->units < result.upper->units;
    return result;
}

namespace {

using StreamCode = generated::StreamCode;
using TableCode = generated::TableCode;
using FieldCode = generated::FieldCode;

// MOEX TEST does not reconcile the order views carried by TRADE and
// USERORDERBOOK.  Keep the surface in the internal key so rows from the two
// streams cannot accidentally become one order or manufacture an identity
// conflict.
enum class OrderSurface : std::uint8_t {
    kUnspecified,
    kTrade,
    kUserOrderbook,
};

struct EnumClassHash {
    template <typename T> std::size_t operator()(T value) const {
        return std::hash<std::uint32_t>{}(static_cast<std::uint32_t>(value));
    }
};

template <typename T> void hash_combine(std::size_t& seed, const T& value) {
    seed ^= std::hash<T>{}(value) + 0x9E3779B97F4A7C15ULL + (seed << 6U) + (seed >> 2U);
}

struct LimitKey {
    LimitParticipantKind participant_kind{LimitParticipantKind::Unknown};
    std::string account_code;
    std::int64_t repl_id{0};
    bool operator==(const LimitKey& other) const {
        if (repl_id != 0 || other.repl_id != 0)
            return repl_id == other.repl_id;
        return participant_kind == other.participant_kind && account_code == other.account_code;
    }
};
struct LimitKeyHash {
    std::size_t operator()(const LimitKey& key) const {
        if (key.repl_id != 0)
            return std::hash<std::int64_t>{}(key.repl_id);
        return std::hash<std::string>{}(key.account_code);
    }
};

struct PositionKey {
    PositionScope scope{PositionScope::kClient};
    std::string account_code;
    std::int32_t isin_id{0};
    std::int8_t account_type{0};

    bool operator==(const PositionKey& other) const {
        return scope == other.scope && account_code == other.account_code && isin_id == other.isin_id &&
               account_type == other.account_type;
    }
};

struct PositionKeyHash {
    std::size_t operator()(const PositionKey& key) const {
        std::size_t seed = 0;
        hash_combine(seed, static_cast<std::uint32_t>(key.scope));
        hash_combine(seed, key.account_code);
        hash_combine(seed, key.isin_id);
        hash_combine(seed, key.account_type);
        return seed;
    }
};

struct OrderKey {
    OrderSurface surface{OrderSurface::kUnspecified};
    bool multileg{false};
    std::int64_t public_order_id{0};
    std::int64_t private_order_id{0};
    std::int32_t ext_id{0};
    std::string client_code;

    bool operator==(const OrderKey& other) const {
        return surface == other.surface && multileg == other.multileg && public_order_id == other.public_order_id &&
               private_order_id == other.private_order_id && ext_id == other.ext_id && client_code == other.client_code;
    }
};

struct OrderKeyHash {
    std::size_t operator()(const OrderKey& key) const {
        std::size_t seed = 0;
        hash_combine(seed, static_cast<std::uint8_t>(key.surface));
        hash_combine(seed, key.multileg);
        hash_combine(seed, key.public_order_id);
        hash_combine(seed, key.private_order_id);
        hash_combine(seed, key.ext_id);
        hash_combine(seed, key.client_code);
        return seed;
    }
};

struct TradeKey {
    bool multileg{false};
    std::int64_t id_deal{0};

    bool operator==(const TradeKey& other) const {
        return multileg == other.multileg && id_deal == other.id_deal;
    }
};

struct TradeKeyHash {
    std::size_t operator()(const TradeKey& key) const {
        std::size_t seed = 0;
        hash_combine(seed, key.multileg);
        hash_combine(seed, key.id_deal);
        return seed;
    }
};

using SessionMap = std::unordered_map<std::int32_t, TradingSessionSnapshot>;
using InstrumentMap = std::unordered_map<std::int32_t, InstrumentSnapshot>;
using FutureSessionMap = std::unordered_map<std::uint64_t, InstrumentSnapshot>;

std::uint64_t future_session_key(std::int32_t isin_id, std::int32_t sess_id) {
    return (std::uint64_t(static_cast<std::uint32_t>(isin_id)) << 32) | static_cast<std::uint32_t>(sess_id);
}

std::int32_t select_session(std::span<const TradingSessionSnapshot> sessions, std::int64_t now_seconds = 0) {
    if (!now_seconds)
        now_seconds =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
    std::int32_t in_window{}, trading{}, known{};
    const auto within = [now_seconds](std::int64_t begin, std::int64_t end) {
        return begin > 0 && end > begin && begin <= now_seconds && now_seconds < end;
    };
    for (const auto& row : sessions) {
        if (!row.has_current_status)
            continue;
        known = std::max(known, row.sess_id);
        if (row.current_status == 1)
            trading = std::max(trading, row.sess_id);
        if (within(row.begin, row.end) || (row.eve_on && within(row.eve_begin, row.eve_end)) ||
            (row.mon_on && within(row.mon_begin, row.mon_end)))
            in_window = std::max(in_window, row.sess_id);
    }
    return in_window ? in_window : (trading ? trading : known);
}
using FutureVcbMap = std::unordered_map<std::string, FutureVcbSnapshot>;
using MatchingMap = std::unordered_map<std::int32_t, MatchingMapSnapshot>;
using SystemMessageMap = std::unordered_map<std::int64_t, SystemMessageSnapshot>;
using LimitMap = std::unordered_map<LimitKey, LimitSnapshot, LimitKeyHash>;
using PositionMap = std::unordered_map<PositionKey, PositionSnapshot, PositionKeyHash>;
struct OrderMap : std::unordered_map<OrderKey, OwnOrderSnapshot, OrderKeyHash> {
    std::unordered_map<std::string, OrderKey> identities;
    std::size_t indexed_count{};
    static std::string identity(const OrderKey& key, char kind, std::int64_t id) {
        return std::to_string(static_cast<int>(key.surface)) + ":" + (key.multileg ? "1:" : "0:") + kind + ":" +
               (kind == 'e' ? key.client_code + ":" : "") + std::to_string(id);
    }
    void index(const OrderKey& key, const OwnOrderSnapshot& row) {
        const auto add = [&](char kind, std::int64_t id) {
            if (id)
                identities[identity(key, kind, id)] = key;
        };
        add('p', row.public_order_id);
        add('v', row.private_order_id);
        add('e', row.ext_id);
        for (auto id : row.public_order_id_aliases)
            add('p', id);
        for (auto id : row.private_order_id_aliases)
            add('v', id);
    }
    void refresh_index() {
        if (indexed_count == size())
            return;
        identities.clear();
        for (const auto& [key, row] : *this)
            index(key, row);
        indexed_count = size();
    }
};
using TradeMap = std::unordered_map<TradeKey, OwnTradeSnapshot, TradeKeyHash>;
using SourceRevisionRows = std::unordered_map<TableCode, std::unordered_map<std::string, std::int64_t>, EnumClassHash>;

struct RowReader {
    std::span<const projection::FieldValueSpec> fields;

    const projection::FieldValueSpec* find(FieldCode code) const {
        for (const auto& field : fields) {
            if (field.field_code == code) {
                return &field;
            }
        }
        return nullptr;
    }

    std::int64_t i64(FieldCode code, std::int64_t fallback = 0) const {
        const auto* field = find(code);
        if (field == nullptr) {
            return fallback;
        }
        if (field->kind == projection::ValueKind::kUnsignedInteger ||
            field->kind == projection::ValueKind::kTimestamp) {
            return static_cast<std::int64_t>(field->unsigned_value);
        }
        return field->signed_value;
    }

    std::uint64_t u64(FieldCode code, std::uint64_t fallback = 0) const {
        const auto* field = find(code);
        if (field == nullptr) {
            return fallback;
        }
        if (field->kind == projection::ValueKind::kSignedInteger) {
            return field->signed_value < 0 ? fallback : static_cast<std::uint64_t>(field->signed_value);
        }
        return field->unsigned_value;
    }

    std::uint64_t timestamp_ns(FieldCode code) const {
        const auto* field = find(code);
        return field ? field->timestamp_ns : 0;
    }

    std::int32_t i32(FieldCode code, std::int32_t fallback = 0) const {
        return static_cast<std::int32_t>(i64(code, fallback));
    }

    std::int8_t i8(FieldCode code, std::int8_t fallback = 0) const {
        return static_cast<std::int8_t>(i64(code, fallback));
    }

    bool boolean(FieldCode code, bool fallback = false) const {
        return i64(code, fallback ? 1 : 0) != 0;
    }

    std::string text(FieldCode code) const {
        const auto* field = find(code);
        return field == nullptr ? std::string{} : std::string(field->text_value);
    }
};

std::string future_vcb_row_key(const RowReader& row) {
    const auto repl_id = row.i64(FieldCode::kFortsRefdataReplFutVcbReplId);
    if (repl_id != 0) {
        return std::string("repl|") + std::to_string(repl_id);
    }
    const auto base_contract_id = row.i32(FieldCode::kFortsRefdataReplFutVcbBaseContractId);
    if (base_contract_id != 0) {
        return std::string("base|") + std::to_string(base_contract_id);
    }
    return std::string("code|") + row.text(FieldCode::kFortsRefdataReplFutVcbBaseContractCode);
}

std::string revision_key(std::initializer_list<std::string_view> parts) {
    std::string key;
    for (const auto part : parts) {
        if (!key.empty()) {
            key.push_back('|');
        }
        key.append(part);
    }
    return key;
}

std::string revision_key(std::int64_t value) {
    return std::to_string(value);
}

std::string revision_key(std::int32_t value) {
    return std::to_string(value);
}

std::string revision_key(const OrderKey& key) {
    return revision_key({key.multileg ? "1" : "0", std::to_string(key.public_order_id),
                         std::to_string(key.private_order_id), std::to_string(key.ext_id), key.client_code});
}

std::string revision_key(const PositionKey& key) {
    return revision_key({std::to_string(static_cast<std::uint32_t>(key.scope)), key.account_code,
                         std::to_string(key.isin_id), std::to_string(key.account_type)});
}

std::string revision_key(const LimitKey& key) {
    if (key.repl_id != 0)
        return revision_key({"repl", std::to_string(key.repl_id)});
    return revision_key({std::to_string(static_cast<std::uint32_t>(key.participant_kind)), key.account_code});
}

std::string revision_key(const OwnOrderSnapshot& row) {
    return revision_key({row.multileg ? "1" : "0", std::to_string(row.public_order_id),
                         std::to_string(row.private_order_id), std::to_string(row.ext_id), row.client_code});
}

std::string revision_key(const OwnTradeSnapshot& row) {
    return revision_key({row.multileg ? "1" : "0", std::to_string(row.id_deal)});
}

template <typename Map> Map& ensure_stage_copy(std::optional<Map>& staged, Map& committed, bool native_commit = false) {
    if (!staged.has_value()) {
        if (native_commit)
            staged = std::move(committed);
        else
            staged = committed;
    }
    return *staged;
}

std::size_t find_stream_index(std::span<const StreamHealthSnapshot> streams, StreamCode stream_code) {
    for (std::size_t index = 0; index < streams.size(); ++index) {
        if (streams[index].stream_code == stream_code) {
            return index;
        }
    }
    return streams.size();
}

template <typename Snapshot, typename Map, typename Comparator>
std::vector<Snapshot> sorted_values(const Map& map, Comparator comparator, std::size_t capacity = 0) {
    std::vector<Snapshot> out;
    out.reserve(std::max(map.size(), capacity));
    for (const auto& [_, value] : map) {
        out.push_back(value);
    }
    std::sort(out.begin(), out.end(), comparator);
    return out;
}

std::size_t growth_capacity(std::size_t size) {
    return size + size / 4 + 1;
}

template <typename Map> void reserve_growth(Map& map) {
    const auto capacity = growth_capacity(map.size());
    if (static_cast<double>(map.bucket_count()) * map.max_load_factor() < capacity)
        map.reserve(capacity);
}

bool order_has_any_source(const OwnOrderSnapshot& order) {
    return order.from_trade_repl || order.from_user_book || order.from_current_day;
}

bool order_change_pending(const OwnOrderSnapshot& order) {
    return order.trade_repl_commit_sequence == std::numeric_limits<std::uint64_t>::max() ||
           order.user_orderbook_commit_sequence == std::numeric_limits<std::uint64_t>::max();
}

void append_identifier_alias(std::vector<std::int64_t>& aliases, std::int64_t value) {
    if (value == 0 || std::find(aliases.begin(), aliases.end(), value) != aliases.end()) {
        return;
    }
    aliases.push_back(value);
    std::sort(aliases.begin(), aliases.end());
}

bool has_identifier_alias(const std::vector<std::int64_t>& aliases, std::int64_t value) {
    return value != 0 && std::find(aliases.begin(), aliases.end(), value) != aliases.end();
}

bool order_identity_matches(const OwnOrderSnapshot& order, const OrderKey& incoming) {
    const bool same_surface = incoming.surface == OrderSurface::kTrade
                                  ? order.from_trade_repl && !order.from_user_book && !order.from_current_day
                              : incoming.surface == OrderSurface::kUserOrderbook
                                  ? !order.from_trade_repl && (order.from_user_book || order.from_current_day)
                                  : true;
    if (!same_surface) {
        return false;
    }
    if (order.multileg != incoming.multileg) {
        return false;
    }
    if (incoming.public_order_id != 0 &&
        (order.public_order_id == incoming.public_order_id ||
         has_identifier_alias(order.public_order_id_aliases, incoming.public_order_id))) {
        return true;
    }
    if (incoming.private_order_id != 0 &&
        (order.private_order_id == incoming.private_order_id ||
         has_identifier_alias(order.private_order_id_aliases, incoming.private_order_id))) {
        return true;
    }
    return incoming.ext_id != 0 && order.ext_id == incoming.ext_id && !incoming.client_code.empty() &&
           order.client_code == incoming.client_code;
}

OwnOrderSnapshot& find_or_create_order(OrderMap& orders, const OrderKey& incoming, OrderKey* resolved_key = nullptr,
                                       bool follow_aliases = true, bool follow_client_reference = true,
                                       bool include_primary_aliases = true) {
    orders.refresh_index();
    auto found = orders.find(incoming);
    const auto lookup = [&](char kind, std::int64_t id) {
        if (found != orders.end() || !id)
            return;
        const auto alias = orders.identities.find(OrderMap::identity(incoming, kind, id));
        if (alias != orders.identities.end())
            found = orders.find(alias->second);
    };
    if (follow_aliases) {
        lookup('p', incoming.public_order_id);
        lookup('v', incoming.private_order_id);
        if (follow_client_reference)
            lookup('e', incoming.ext_id);
    }
    if (found == orders.end())
        found = orders.emplace(incoming, OwnOrderSnapshot{}).first;
    auto& order = found->second;

    if (include_primary_aliases) {
        // Legacy synthetic projections coalesce client ext_id references and
        // expose primary IDs in alias lists as part of their existing contract.
        append_identifier_alias(order.public_order_id_aliases, order.public_order_id);
        append_identifier_alias(order.private_order_id_aliases, order.private_order_id);
        append_identifier_alias(order.public_order_id_aliases, incoming.public_order_id);
        append_identifier_alias(order.private_order_id_aliases, incoming.private_order_id);
    } else {
        // Native exchange rows keep separate IDs when Move reuses ext_id;
        // canonical IDs are already stored and indexed as scalars, so only
        // actual alternate exchange IDs belong in their alias lists.
        // Allocating alias vectors for those same IDs fragments the allocator
        // when a large bootstrap delta is copied and then released.
        if (order.public_order_id && order.public_order_id != incoming.public_order_id)
            append_identifier_alias(order.public_order_id_aliases, incoming.public_order_id);
        if (order.private_order_id && order.private_order_id != incoming.private_order_id)
            append_identifier_alias(order.private_order_id_aliases, incoming.private_order_id);
    }

    const bool same_client_ext = incoming.ext_id != 0 && order.ext_id == incoming.ext_id &&
                                 !incoming.client_code.empty() && order.client_code == incoming.client_code;
    if (same_client_ext && ((order.public_order_id != 0 && incoming.public_order_id != 0 &&
                             order.public_order_id != incoming.public_order_id) ||
                            (order.private_order_id != 0 && incoming.private_order_id != 0 &&
                             order.private_order_id != incoming.private_order_id))) {
        order.identity_conflict = true;
    }

    if (order.public_order_id == 0) {
        order.public_order_id = incoming.public_order_id;
    }
    if (order.private_order_id == 0) {
        order.private_order_id = incoming.private_order_id;
    }
    if (order.ext_id == 0) {
        order.ext_id = incoming.ext_id;
    }
    if (order.client_code.empty()) {
        order.client_code = incoming.client_code;
    }
    order.multileg = incoming.multileg;
    orders.index(found->first, order);
    orders.indexed_count = orders.size();
    if (resolved_key)
        *resolved_key = found->first;
    return order;
}

void clear_trade_source(OrderMap& orders) {
    for (auto it = orders.begin(); it != orders.end();) {
        it->second.from_trade_repl = false;
        it->second.trade_repl_commit_sequence = 0;
        if (!order_has_any_source(it->second)) {
            it = orders.erase(it);
        } else {
            ++it;
        }
    }
}

void clear_user_book_source(OrderMap& orders) {
    for (auto it = orders.begin(); it != orders.end();) {
        it->second.from_user_book = false;
        it->second.from_current_day = false;
        it->second.user_orderbook_commit_sequence = 0;
        if (!order_has_any_source(it->second)) {
            it = orders.erase(it);
        } else {
            ++it;
        }
    }
}

void append_or_replace_leg(std::vector<InstrumentLegSnapshot>& legs, InstrumentLegSnapshot leg) {
    for (auto& existing : legs) {
        if (existing.leg_isin_id == leg.leg_isin_id && existing.leg_order_no == leg.leg_order_no) {
            existing = std::move(leg);
            return;
        }
    }
    legs.push_back(std::move(leg));
    std::sort(legs.begin(), legs.end(), [](const InstrumentLegSnapshot& lhs, const InstrumentLegSnapshot& rhs) {
        if (lhs.leg_order_no != rhs.leg_order_no) {
            return lhs.leg_order_no < rhs.leg_order_no;
        }
        return lhs.leg_isin_id < rhs.leg_isin_id;
    });
}

struct StagedState {
    bool status_bindings_invalidated{false};
    bool active{false};
    std::optional<SessionMap> sessions;
    std::optional<InstrumentMap> instruments;
    std::optional<FutureSessionMap> future_sessions;
    std::optional<FutureVcbMap> future_vcb;
    std::optional<MatchingMap> matching_map;
    std::optional<SystemMessageMap> system_messages;
    std::optional<LimitMap> limits;
    std::optional<PositionMap> positions;
    std::optional<OrderMap> orders;
    std::optional<TradeMap> trades;
    // Contiguous transient keys avoid retaining a bootstrap-sized node pool,
    // or returning thousands of small nodes to the allocator at each commit.
    // Order commit sentinels deduplicate touches; trade keys are uniqued once.
    std::vector<OrderKey> order_keys;
    std::vector<TradeKey> trade_keys;
    bool private_history_retired{false};
    bool rebuild_order_view{}, rebuild_trade_view{};
    std::optional<std::vector<StreamHealthSnapshot>> stream_health;
    std::optional<SourceRevisionRows> source_revisions;
    std::unordered_set<StreamCode, EnumClassHash> touched_streams;
    bool userbook_regular_info_seen{false};
    std::int32_t userbook_regular_info_publication_state{0};
};

void reset_stream_watermarks(StreamHealthSnapshot& health) {
    health.committed_row_count = 0;
    health.last_commit_sequence = 0;
    health.has_publication_state = false;
    health.publication_state = 0;
    health.last_trades_rev = 0;
    health.last_trades_lifenum = 0;
    health.last_server_time = 0;
    health.last_info_moment = 0;
    health.last_event_id = 0;
    health.last_event_type = 0;
    health.last_message.clear();
    health.periodic_snapshot_consistent = false;
}

bool is_regular_userorderbook_snapshot_table(TableCode table_code) {
    switch (table_code) {
    case TableCode::kFortsUserorderbookReplOrders:
    case TableCode::kFortsUserorderbookReplMultilegOrders:
    case TableCode::kFortsUserorderbookReplInfo:
        return true;
    default:
        return false;
    }
}

} // namespace

struct Plaza2PrivateStateProjector::Impl {
    ConnectorHealthSnapshot connector_health;
    ResumeMarkersSnapshot resume_markers;
    std::vector<StreamHealthSnapshot> stream_health;

    SessionMap sessions_by_id;
    InstrumentMap instruments_by_isin;
    FutureSessionMap future_sessions;
    FutureVcbMap future_vcb_by_row;
    MatchingMap matching_by_base_contract;
    SystemMessageMap system_messages_by_id;
    LimitMap limits_by_key;
    std::unordered_map<std::string, LimitLookup> limit_index;
    std::size_t unknown_limits{0};
    PositionMap positions_by_key;
    OrderMap orders_by_key;
    TradeMap trades_by_key;
    SourceRevisionRows source_revisions;
    std::unordered_map<StreamCode, std::uint64_t, EnumClassHash> lifenums_by_stream;

    std::vector<TradingSessionSnapshot> session_snapshots;
    std::vector<InstrumentSnapshot> instrument_snapshots;
    std::vector<FutureVcbSnapshot> future_vcb_snapshots;
    std::vector<MatchingMapSnapshot> matching_snapshots;
    std::vector<SystemMessageSnapshot> system_message_snapshots;
    std::vector<LimitSnapshot> limit_snapshots;
    std::vector<PositionSnapshot> position_snapshots;
    std::vector<OwnOrderSnapshot> order_snapshots;
    std::vector<OwnTradeSnapshot> trade_snapshots;
    std::unordered_map<OrderKey, std::size_t, OrderKeyHash> order_view_index;
    std::unordered_map<TradeKey, std::size_t, TradeKeyHash> trade_view_index;
    PrivateRowChanges row_changes;

    StagedState staged;
    bool native_commit_phase{false};
    std::uint64_t status_binding_generation{0};

    void rebind_limit_index() {
        // A default copy owns its maps and pending transaction. Only these
        // cached pointers need rebinding while the source is still alive.
        const auto& limits = native_commit_phase && staged.limits ? *staged.limits : limits_by_key;
        for (auto& [code, lookup] : limit_index) {
            if (lookup.exact) {
                const auto& row = *lookup.exact;
                const auto found = limits.find(LimitKey{row.participant_kind, row.account_code, row.repl_id});
                lookup.exact = found == limits.end() ? nullptr : &found->second;
            }
        }
    }

    void invalidate_status_bindings() {
        if (staged.active)
            staged.status_bindings_invalidated = true;
        else
            ++status_binding_generation;
    }

    void reset() {
        ++status_binding_generation;
        connector_health = {};
        resume_markers = {};
        stream_health.clear();
        sessions_by_id.clear();
        instruments_by_isin.clear();
        future_sessions.clear();
        future_vcb_by_row.clear();
        matching_by_base_contract.clear();
        system_messages_by_id.clear();
        limits_by_key.clear();
        limit_index.clear();
        unknown_limits = 0;
        positions_by_key.clear();
        orders_by_key.clear();
        trades_by_key.clear();
        source_revisions.clear();
        lifenums_by_stream.clear();
        session_snapshots.clear();
        instrument_snapshots.clear();
        future_vcb_snapshots.clear();
        matching_snapshots.clear();
        system_message_snapshots.clear();
        limit_snapshots.clear();
        position_snapshots.clear();
        order_snapshots.clear();
        trade_snapshots.clear();
        order_view_index.clear();
        trade_view_index.clear();
        row_changes = {};
        staged = {};
    }

    void sync_base_health(const projection::EngineState& state) {
        connector_health.open = state.open;
        connector_health.closed = state.closed;
        connector_health.snapshot_active = state.snapshot_active;
        connector_health.online = state.online;
        connector_health.transaction_open = state.transaction_open;
        connector_health.commit_count = state.commit_count;
        connector_health.callback_error_count = state.callback_error_count;
        resume_markers.has_lifenum = state.has_lifenum;
        resume_markers.last_lifenum = state.last_lifenum;
        resume_markers.last_replstate = state.last_replstate;

        for (const auto& engine_stream : state.streams) {
            const auto index = find_stream_index(stream_health, engine_stream.stream_code);
            if (index == stream_health.size()) {
                stream_health.push_back({
                    .stream_code = engine_stream.stream_code,
                    .stream_name = std::string(engine_stream.stream_name),
                });
            }
            auto& snapshot = stream_health[find_stream_index(stream_health, engine_stream.stream_code)];
            snapshot.online = engine_stream.online;
            snapshot.snapshot_complete = engine_stream.snapshot_complete;
            snapshot.clear_deleted_count = engine_stream.clear_deleted_count;
            snapshot.committed_row_count = engine_stream.committed_row_count;
        }
        std::sort(stream_health.begin(), stream_health.end(),
                  [](const StreamHealthSnapshot& lhs, const StreamHealthSnapshot& rhs) {
                      return static_cast<std::uint32_t>(lhs.stream_code) < static_cast<std::uint32_t>(rhs.stream_code);
                  });
    }

    void invalidate_periodic_snapshot(StreamCode stream_code, TableCode table_code) {
        if (stream_code != StreamCode::kFortsUserorderbookRepl ||
            !is_regular_userorderbook_snapshot_table(table_code)) {
            return;
        }
        auto& target = staged.active ? ensure_staged_stream_health() : stream_health;
        auto& health = ensure_stream_health(target, stream_code);
        health.periodic_snapshot_consistent = false;
        if (staged.active) {
            staged.userbook_regular_info_seen = false;
        }
    }

    void invalidate_closed_stream(StreamCode stream_code) {
        discard_stream_row_changes(stream_code, true);
        if (stream_code == projection::kNoStreamCode) {
            const auto resync_required = row_changes.resync_required;
            const auto history_lost = row_changes.history_lost;
            row_changes = {.resync_required = resync_required, .history_lost = history_lost};
        }
        const auto invalidate = [](StreamHealthSnapshot& health) {
            health.online = false;
            health.snapshot_complete = false;
            health.periodic_snapshot_consistent = false;
        };
        if (stream_code == projection::kNoStreamCode) {
            for (auto& health : stream_health) {
                invalidate(health);
            }
            return;
        }
        invalidate(ensure_stream_health(stream_health, stream_code));
    }

    std::vector<StreamHealthSnapshot>& ensure_staged_stream_health() {
        if (!staged.stream_health.has_value()) {
            if (native_commit_phase)
                staged.stream_health = std::move(stream_health);
            else
                staged.stream_health = stream_health;
        }
        return *staged.stream_health;
    }

    SourceRevisionRows& ensure_staged_source_revisions() {
        if (!staged.source_revisions.has_value()) {
            if (native_commit_phase)
                staged.source_revisions = std::move(source_revisions);
            else
                staged.source_revisions = source_revisions;
        }
        return *staged.source_revisions;
    }

    SourceRevisionRows& active_source_revisions() {
        return staged.active ? ensure_staged_source_revisions() : source_revisions;
    }

    std::optional<SourceRowProvenance> refdata_source_provenance(TableCode table_code, std::int32_t row_id) const {
        if (table_code == TableCode::kFortsRefdataReplFutSessContents) {
            const auto instrument = instruments_by_isin.find(row_id);
            if (instrument == instruments_by_isin.end() || !instrument->second.future_session_terms)
                return std::nullopt;
            return instrument->second.future_session_terms->source;
        }
        switch (table_code) {
        case TableCode::kFortsRefdataReplFutInstruments:
        case TableCode::kFortsRefdataReplFutSessContents:
        case TableCode::kFortsRefdataReplSession:
            break;
        default:
            return std::nullopt;
        }

        const auto lifenum = lifenums_by_stream.find(StreamCode::kFortsRefdataRepl);
        if (lifenum == lifenums_by_stream.end()) {
            return std::nullopt;
        }
        const auto table_it = source_revisions.find(table_code);
        if (table_it == source_revisions.end()) {
            return std::nullopt;
        }
        const auto row_it = table_it->second.find(revision_key(row_id));
        if (row_it == table_it->second.end()) {
            return std::nullopt;
        }
        return SourceRowProvenance{
            .stream_code = StreamCode::kFortsRefdataRepl,
            .table_code = table_code,
            .repl_rev = row_it->second,
            .lifenum = lifenum->second,
            .present = true,
        };
    }

    std::optional<std::uint64_t> refdata_lifenum() const {
        const auto lifenum = lifenums_by_stream.find(StreamCode::kFortsRefdataRepl);
        if (lifenum == lifenums_by_stream.end()) {
            return std::nullopt;
        }
        return lifenum->second;
    }

    void record_source_revision(TableCode table_code, std::string key, std::int64_t revision) {
        if (key.empty()) {
            return;
        }
        active_source_revisions()[table_code][std::move(key)] = revision;
    }

    std::string row_revision_key(TableCode table_code, const RowReader& row) const {
        switch (table_code) {
        case TableCode::kFortsTradeReplOrdersLog:
            return revision_key(OrderKey{
                .multileg = false,
                .public_order_id = row.i64(FieldCode::kFortsTradeReplOrdersLogPublicOrderId),
                .private_order_id = row.i64(FieldCode::kFortsTradeReplOrdersLogPrivateOrderId),
                .ext_id = row.i32(FieldCode::kFortsTradeReplOrdersLogExtId),
                .client_code = row.text(FieldCode::kFortsTradeReplOrdersLogClientCode),
            });
        case TableCode::kFortsTradeReplMultilegOrdersLog:
            return revision_key(OrderKey{
                .multileg = true,
                .public_order_id = row.i64(FieldCode::kFortsTradeReplMultilegOrdersLogPublicOrderId),
                .private_order_id = row.i64(FieldCode::kFortsTradeReplMultilegOrdersLogPrivateOrderId),
                .ext_id = row.i32(FieldCode::kFortsTradeReplMultilegOrdersLogExtId),
                .client_code = row.text(FieldCode::kFortsTradeReplMultilegOrdersLogClientCode),
            });
        case TableCode::kFortsTradeReplUserDeal:
            return revision_key({"0", std::to_string(row.i64(FieldCode::kFortsTradeReplUserDealIdDeal))});
        case TableCode::kFortsTradeReplUserMultilegDeal:
            return revision_key({"1", std::to_string(row.i64(FieldCode::kFortsTradeReplUserMultilegDealIdDeal))});
        case TableCode::kFortsUserorderbookReplOrders:
            return revision_key(OrderKey{
                .multileg = false,
                .public_order_id = row.i64(FieldCode::kFortsUserorderbookReplOrdersPublicOrderId),
                .private_order_id = row.i64(FieldCode::kFortsUserorderbookReplOrdersPrivateOrderId),
                .ext_id = row.i32(FieldCode::kFortsUserorderbookReplOrdersExtId),
                .client_code = row.text(FieldCode::kFortsUserorderbookReplOrdersClientCode),
            });
        case TableCode::kFortsUserorderbookReplMultilegOrders:
            return revision_key(OrderKey{
                .multileg = true,
                .public_order_id = row.i64(FieldCode::kFortsUserorderbookReplMultilegOrdersPublicOrderId),
                .private_order_id = row.i64(FieldCode::kFortsUserorderbookReplMultilegOrdersPrivateOrderId),
                .ext_id = row.i32(FieldCode::kFortsUserorderbookReplMultilegOrdersExtId),
                .client_code = row.text(FieldCode::kFortsUserorderbookReplMultilegOrdersClientCode),
            });
        case TableCode::kFortsUserorderbookReplOrdersCurrentday:
            return revision_key(OrderKey{
                .multileg = false,
                .public_order_id = row.i64(FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicOrderId),
                .private_order_id = row.i64(FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateOrderId),
                .ext_id = row.i32(FieldCode::kFortsUserorderbookReplOrdersCurrentdayExtId),
                .client_code = row.text(FieldCode::kFortsUserorderbookReplOrdersCurrentdayClientCode),
            });
        case TableCode::kFortsUserorderbookReplMultilegOrdersCurrentday:
            return revision_key(OrderKey{
                .multileg = true,
                .public_order_id = row.i64(FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayPublicOrderId),
                .private_order_id = row.i64(FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayPrivateOrderId),
                .ext_id = row.i32(FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayExtId),
                .client_code = row.text(FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayClientCode),
            });
        case TableCode::kFortsPosReplPosition:
            return revision_key(PositionKey{
                .scope = PositionScope::kClient,
                .account_code = row.text(FieldCode::kFortsPosReplPositionClientCode),
                .isin_id = row.i32(FieldCode::kFortsPosReplPositionIsinId),
                .account_type = row.i8(FieldCode::kFortsPosReplPositionAccountType),
            });
        case TableCode::kFortsPartReplPart:
            return revision_key(LimitKey{
                .participant_kind = classify_limit_participant(row.text(FieldCode::kFortsPartReplPartClientCode)),
                .account_code = row.text(FieldCode::kFortsPartReplPartClientCode),
                .repl_id = row.i64(FieldCode::kFortsPartReplPartReplId),
            });
        case TableCode::kFortsRefdataReplSession:
            return revision_key(row.i32(FieldCode::kFortsRefdataReplSessionSessId));
        case TableCode::kFortsRefdataReplFutInstruments:
            return revision_key(row.i32(FieldCode::kFortsRefdataReplFutInstrumentsIsinId));
        case TableCode::kFortsRefdataReplFutSessContents:
            return revision_key({std::to_string(row.i32(FieldCode::kFortsRefdataReplFutSessContentsIsinId)),
                                 std::to_string(row.i32(FieldCode::kFortsRefdataReplFutSessContentsSessId))});
        case TableCode::kFortsRefdataReplFutVcb:
            return future_vcb_row_key(row);
        case TableCode::kFortsRefdataReplOptSessContents:
            return revision_key(row.i32(FieldCode::kFortsRefdataReplOptSessContentsIsinId));
        case TableCode::kFortsRefdataReplMultilegDict:
            return revision_key({std::to_string(row.i32(FieldCode::kFortsRefdataReplMultilegDictIsinId)),
                                 std::to_string(row.i8(FieldCode::kFortsRefdataReplMultilegDictLegOrderNo))});
        case TableCode::kFortsRefdataReplInstr2matchingMap:
            return revision_key(row.i32(FieldCode::kFortsRefdataReplInstr2matchingMapBaseContractId));
        case TableCode::kFortsRefdataReplSysMessages:
            return revision_key(row.i64(FieldCode::kFortsRefdataReplSysMessagesReplId));
        case TableCode::kFortsSessionstateReplSessionState:
            return revision_key(row.i32(FieldCode::kFortsSessionstateReplSessionStateSessId));
        case TableCode::kFortsInstrumentstateReplInstrumentState:
            return revision_key(row.i32(FieldCode::kFortsInstrumentstateReplInstrumentStateIsinId));
        default:
            return {};
        }
    }

    StreamHealthSnapshot& ensure_stream_health(std::vector<StreamHealthSnapshot>& target, StreamCode stream_code) {
        const auto index = find_stream_index(target, stream_code);
        if (index != target.size()) {
            return target[index];
        }
        const auto* descriptor = generated::FindStreamByCode(stream_code);
        target.push_back({
            .stream_code = stream_code,
            .stream_name = descriptor != nullptr ? std::string(descriptor->stream_name) : std::string{},
        });
        return target.back();
    }

    void rebuild_sessions() {
        session_snapshots = sorted_values<TradingSessionSnapshot>(
            sessions_by_id, [](const TradingSessionSnapshot& lhs, const TradingSessionSnapshot& rhs) {
                return lhs.sess_id < rhs.sess_id;
            });
    }

    void rebuild_future_vcb() {
        future_vcb_snapshots = sorted_values<FutureVcbSnapshot>(
            future_vcb_by_row, [](const FutureVcbSnapshot& lhs, const FutureVcbSnapshot& rhs) {
                if (lhs.base_contract_code != rhs.base_contract_code)
                    return lhs.base_contract_code < rhs.base_contract_code;
                if (lhs.base_contract_id != rhs.base_contract_id)
                    return lhs.base_contract_id < rhs.base_contract_id;
                return lhs.repl_id < rhs.repl_id;
            });
    }

    void join_future_vcb_into_instruments() {
        for (auto& [unused_isin_id, instrument] : instruments_by_isin) {
            static_cast<void>(unused_isin_id);
            instrument.base_contract_id = 0;
            instrument.future_vcb_join_status = FutureVcbJoinStatus::Missing;
            instrument.future_vcb_currency.clear();
            instrument.future_vcb_board_md.clear();
            instrument.future_vcb_provenance = {};
            if (instrument.kind != InstrumentKind::kFuture || instrument.base_contract_code.empty())
                continue;

            std::size_t match_count = 0;
            const FutureVcbSnapshot* match = nullptr;
            for (const auto& [unused_key, row] : future_vcb_by_row) {
                static_cast<void>(unused_key);
                if (row.base_contract_code != instrument.base_contract_code)
                    continue;
                ++match_count;
                if (match_count == 1)
                    match = &row;
            }
            if (match_count != 1) {
                instrument.future_vcb_join_status =
                    match_count > 1 ? FutureVcbJoinStatus::Ambiguous : FutureVcbJoinStatus::Missing;
                continue;
            }

            // The locked schema supplies the join identity, quotation currency,
            // ASTS SECBOARD and row provenance. Any missing piece is not a
            // partially usable definition: it remains fail-closed.
            if (match == nullptr || match->base_contract_id <= 0 || match->currency.empty() ||
                match->board_md.empty() || !match->source.present) {
                continue;
            }
            instrument.base_contract_id = match->base_contract_id;
            instrument.future_vcb_join_status = FutureVcbJoinStatus::Resolved;
            instrument.future_vcb_currency = match->currency;
            instrument.future_vcb_board_md = match->board_md;
            instrument.future_vcb_provenance = match->source;
        }
    }

    void rebuild_instruments() {
        join_future_vcb_into_instruments();
        instrument_snapshots = sorted_values<InstrumentSnapshot>(
            instruments_by_isin,
            [](const InstrumentSnapshot& lhs, const InstrumentSnapshot& rhs) { return lhs.isin_id < rhs.isin_id; });
    }

    void select_future_session_membership() {
        const auto current = select_session(session_snapshots);
        if (!current)
            return;
        for (auto& [isin_id, instrument] : instruments_by_isin) {
            const auto selected = future_sessions.find(future_session_key(isin_id, current));
            if (selected == future_sessions.end() || instrument.sess_id == current)
                continue;
            const auto has_status = instrument.has_current_status;
            const auto status = instrument.current_status;
            instrument = selected->second;
            instrument.has_current_status = has_status;
            instrument.current_status = status;
            instrument.current_status_refdata_bound = false;
            invalidate_status_bindings();
        }
    }

    void rebuild_matching_map() {
        matching_snapshots = sorted_values<MatchingMapSnapshot>(
            matching_by_base_contract, [](const MatchingMapSnapshot& lhs, const MatchingMapSnapshot& rhs) {
                return lhs.base_contract_id < rhs.base_contract_id;
            });
    }

    void rebuild_system_messages() {
        system_message_snapshots = sorted_values<SystemMessageSnapshot>(
            system_messages_by_id, [](const SystemMessageSnapshot& lhs, const SystemMessageSnapshot& rhs) {
                return lhs.repl_id < rhs.repl_id;
            });
    }

    void rebuild_limits() {
        limit_snapshots =
            sorted_values<LimitSnapshot>(limits_by_key, [](const LimitSnapshot& lhs, const LimitSnapshot& rhs) {
                if (lhs.participant_kind != rhs.participant_kind) {
                    return static_cast<std::uint32_t>(lhs.participant_kind) <
                           static_cast<std::uint32_t>(rhs.participant_kind);
                }
                if (lhs.account_code != rhs.account_code)
                    return lhs.account_code < rhs.account_code;
                return lhs.repl_id < rhs.repl_id;
            });
        limit_index.clear();
        unknown_limits = 0;
        for (const auto& [key, row] : limits_by_key) {
            auto& entry = limit_index[row.account_code];
            ++entry.match_count;
            entry.exact = entry.match_count == 1 ? &row : nullptr;
            unknown_limits += row.participant_kind == LimitParticipantKind::Unknown;
        }
    }

    void rebuild_positions() {
        position_snapshots = sorted_values<PositionSnapshot>(
            positions_by_key, [](const PositionSnapshot& lhs, const PositionSnapshot& rhs) {
                if (lhs.scope != rhs.scope) {
                    return static_cast<std::uint32_t>(lhs.scope) < static_cast<std::uint32_t>(rhs.scope);
                }
                if (lhs.account_code != rhs.account_code) {
                    return lhs.account_code < rhs.account_code;
                }
                if (lhs.isin_id != rhs.isin_id) {
                    return lhs.isin_id < rhs.isin_id;
                }
                return lhs.account_type < rhs.account_type;
            });
    }

    void reserve_revision_growth(std::initializer_list<TableCode> tables) {
        auto& revisions = active_source_revisions();
        for (const auto table : tables) {
            const auto found = revisions.find(table);
            if (found != revisions.end())
                reserve_growth(found->second);
        }
    }

    void rebuild_orders() {
        // Reserve every large index at the bulk boundary. Online insertions
        // must not depend on the standard library's bucket growth thresholds.
        reserve_growth(orders_by_key);
        orders_by_key.refresh_index();
        reserve_growth(orders_by_key.identities);
        reserve_revision_growth({TableCode::kFortsTradeReplOrdersLog, TableCode::kFortsTradeReplMultilegOrdersLog,
                                 TableCode::kFortsUserorderbookReplOrders,
                                 TableCode::kFortsUserorderbookReplMultilegOrders,
                                 TableCode::kFortsUserorderbookReplOrdersCurrentday,
                                 TableCode::kFortsUserorderbookReplMultilegOrdersCurrentday});
        order_snapshots = sorted_values<OwnOrderSnapshot>(
            orders_by_key,
            [](const OwnOrderSnapshot& lhs, const OwnOrderSnapshot& rhs) {
                if (lhs.multileg != rhs.multileg) {
                    return lhs.multileg < rhs.multileg;
                }
                if (lhs.public_order_id != rhs.public_order_id) {
                    return lhs.public_order_id < rhs.public_order_id;
                }
                if (lhs.private_order_id != rhs.private_order_id) {
                    return lhs.private_order_id < rhs.private_order_id;
                }
                if (lhs.client_code != rhs.client_code) {
                    return lhs.client_code < rhs.client_code;
                }
                if (lhs.ext_id != rhs.ext_id) {
                    return lhs.ext_id < rhs.ext_id;
                }
                // Same identifiers can legitimately occur on the two
                // independent MOEX TEST surfaces. Keep their order
                // deterministic without coalescing their evidence.
                if (lhs.from_trade_repl != rhs.from_trade_repl) {
                    return lhs.from_trade_repl > rhs.from_trade_repl;
                }
                if (lhs.from_user_book != rhs.from_user_book) {
                    return lhs.from_user_book > rhs.from_user_book;
                }
                return lhs.from_current_day > rhs.from_current_day;
            },
            growth_capacity(orders_by_key.size()));
        order_view_index.clear();
        order_view_index.reserve(growth_capacity(order_snapshots.size()));
        for (std::size_t index = 0; index < order_snapshots.size(); ++index) {
            const auto& row = order_snapshots[index];
            const OrderKey key{.surface = row.from_trade_repl ? OrderSurface::kTrade : OrderSurface::kUserOrderbook,
                               .multileg = row.multileg,
                               .public_order_id = row.public_order_id,
                               .private_order_id = row.private_order_id,
                               .ext_id = row.ext_id,
                               .client_code = row.client_code};
            const auto found = orders_by_key.identities.find(OrderMap::identity(key, 'p', row.public_order_id));
            order_view_index[found == orders_by_key.identities.end() ? key : found->second] = index;
        }
    }

    void rebuild_trades() {
        reserve_growth(trades_by_key);
        reserve_revision_growth({TableCode::kFortsTradeReplUserDeal, TableCode::kFortsTradeReplUserMultilegDeal});
        trade_snapshots = sorted_values<OwnTradeSnapshot>(
            trades_by_key,
            [](const OwnTradeSnapshot& lhs, const OwnTradeSnapshot& rhs) {
                if (lhs.multileg != rhs.multileg) {
                    return lhs.multileg < rhs.multileg;
                }
                return lhs.id_deal < rhs.id_deal;
            },
            growth_capacity(trades_by_key.size()));
        trade_view_index.clear();
        trade_view_index.reserve(growth_capacity(trade_snapshots.size()));
        for (std::size_t index = 0; index < trade_snapshots.size(); ++index) {
            const auto& row = trade_snapshots[index];
            trade_view_index[{row.multileg, row.id_deal}] = index;
        }
    }

    void rebuild_all_snapshots() {
        rebuild_sessions();
        rebuild_future_vcb();
        rebuild_instruments();
        rebuild_matching_map();
        rebuild_system_messages();
        rebuild_limits();
        rebuild_positions();
        rebuild_orders();
        rebuild_trades();
    }

    void prune_row_changes() {
        const auto erased_orders = std::erase_if(row_changes.orders, [&](const auto& row) {
            const OrderKey key{.surface = row.from_trade_repl ? OrderSurface::kTrade : OrderSurface::kUserOrderbook,
                               .multileg = row.multileg,
                               .public_order_id = row.public_order_id,
                               .private_order_id = row.private_order_id,
                               .ext_id = row.ext_id,
                               .client_code = row.client_code};
            // A reused ext_id must not keep a deleted previous-day row alive.
            // Exchange identifiers take precedence over the client reference.
            if (row.public_order_id)
                return !orders_by_key.identities.contains(OrderMap::identity(key, 'p', row.public_order_id));
            if (row.private_order_id)
                return !orders_by_key.identities.contains(OrderMap::identity(key, 'v', row.private_order_id));
            if (row.ext_id)
                return !orders_by_key.identities.contains(OrderMap::identity(key, 'e', row.ext_id));
            return !orders_by_key.contains(key);
        });
        const auto erased_trades = std::erase_if(
            row_changes.trades, [&](const auto& row) { return !trades_by_key.contains({row.multileg, row.id_deal}); });
        if (erased_orders || erased_trades)
            require_history_resync();
    }

    void require_history_resync() {
        row_changes = {.resync_required = true, .history_lost = true};
    }

    void discard_stream_row_changes(StreamCode stream_code, bool invalidated = false) {
        if (stream_code != projection::kNoStreamCode && stream_code != StreamCode::kFortsTradeRepl &&
            stream_code != StreamCode::kFortsUserorderbookRepl)
            return;
        const auto discarded_orders =
            std::any_of(row_changes.orders.begin(), row_changes.orders.end(), [&](const auto& row) {
                return stream_code == projection::kNoStreamCode ||
                       (stream_code == StreamCode::kFortsTradeRepl ? row.from_trade_repl : !row.from_trade_repl);
            });
        const bool discarded_trades = stream_code != StreamCode::kFortsUserorderbookRepl && !row_changes.trades.empty();
        if ((invalidated && row_changes.resync_required) || discarded_orders || discarded_trades)
            require_history_resync();
    }

    void retire_private_history() {
        if (staged.active)
            staged.private_history_retired = true;
        else
            row_changes.history_lost |= row_changes.resync_required;
    }

    bool prepare_row_changes(std::size_t additional_rows) {
        if (row_changes.resync_required)
            return false;
        const auto pending = row_changes.orders.size() + row_changes.trades.size();
        if (additional_rows <= kPrivateRowChangeCapacity - pending)
            return true;
        row_changes = {.resync_required = true, .history_lost = staged.private_history_retired};
        return false;
    }

    bool source_row_is_stale(TableCode table_code, std::string_view key, std::int64_t clear_revision) const {
        const SourceRevisionRows* revisions = &source_revisions;
        if (staged.active) {
            if (!staged.source_revisions.has_value()) {
                return false;
            }
            revisions = &*staged.source_revisions;
        }
        const auto rows_it = revisions->find(table_code);
        if (rows_it == revisions->end()) {
            return false;
        }
        const auto row_it = rows_it->second.find(std::string(key));
        if (row_it == rows_it->second.end()) {
            return false;
        }
        return clear_revision == std::numeric_limits<std::int64_t>::max() || row_it->second < clear_revision;
    }

    bool has_source_row(TableCode table_code, std::string_view key) const {
        const SourceRevisionRows* revisions = &source_revisions;
        if (staged.active) {
            if (!staged.source_revisions.has_value()) {
                return false;
            }
            revisions = &*staged.source_revisions;
        }
        const auto table_it = revisions->find(table_code);
        if (table_code == TableCode::kFortsRefdataReplFutSessContents && key.find('|') == std::string_view::npos &&
            table_it != revisions->end()) {
            const auto prefix = std::string(key) + '|';
            return std::any_of(table_it->second.begin(), table_it->second.end(),
                               [&](const auto& row) { return row.first.starts_with(prefix); });
        }
        return table_it != revisions->end() && table_it->second.find(std::string(key)) != table_it->second.end();
    }

    void erase_source_row(TableCode table_code, std::string_view key) {
        auto& revisions = active_source_revisions();
        const auto table_it = revisions.find(table_code);
        if (table_it == revisions.end()) {
            return;
        }
        table_it->second.erase(std::string(key));
        if (table_it->second.empty()) {
            revisions.erase(table_it);
        }
    }

    void clear_table_owned_state(TableCode table_code, std::int64_t clear_revision) {
        using enum TableCode;
        if (table_code == kFortsTradeReplOrdersLog || table_code == kFortsTradeReplMultilegOrdersLog ||
            table_code == kFortsUserorderbookReplOrders || table_code == kFortsUserorderbookReplMultilegOrders ||
            table_code == kFortsUserorderbookReplOrdersCurrentday ||
            table_code == kFortsUserorderbookReplMultilegOrdersCurrentday)
            staged.rebuild_order_view = true;
        if (table_code == kFortsTradeReplUserDeal || table_code == kFortsTradeReplUserMultilegDeal)
            staged.rebuild_trade_view = true;
        static_cast<void>(active_source_revisions());
        auto& sessions =
            staged.active ? ensure_stage_copy(staged.sessions, sessions_by_id, native_commit_phase) : sessions_by_id;
        auto& instruments = staged.active
                                ? ensure_stage_copy(staged.instruments, instruments_by_isin, native_commit_phase)
                                : instruments_by_isin;
        auto& future_vcb = staged.active ? ensure_stage_copy(staged.future_vcb, future_vcb_by_row, native_commit_phase)
                                         : future_vcb_by_row;
        auto& matching = staged.active
                             ? ensure_stage_copy(staged.matching_map, matching_by_base_contract, native_commit_phase)
                             : matching_by_base_contract;
        auto& system_messages =
            staged.active ? ensure_stage_copy(staged.system_messages, system_messages_by_id, native_commit_phase)
                          : system_messages_by_id;
        auto& limits =
            staged.active ? ensure_stage_copy(staged.limits, limits_by_key, native_commit_phase) : limits_by_key;
        auto& positions = staged.active ? ensure_stage_copy(staged.positions, positions_by_key, native_commit_phase)
                                        : positions_by_key;
        auto& orders =
            staged.active ? ensure_stage_copy(staged.orders, orders_by_key, native_commit_phase) : orders_by_key;
        auto& trades =
            staged.active ? ensure_stage_copy(staged.trades, trades_by_key, native_commit_phase) : trades_by_key;

        const auto clear_stream_for_table = [&](TableCode code) {
            switch (code) {
            case TableCode::kFortsTradeReplOrdersLog:
            case TableCode::kFortsTradeReplMultilegOrdersLog:
            case TableCode::kFortsTradeReplUserDeal:
            case TableCode::kFortsTradeReplUserMultilegDeal:
                return StreamCode::kFortsTradeRepl;
            case TableCode::kFortsUserorderbookReplOrders:
            case TableCode::kFortsUserorderbookReplMultilegOrders:
            case TableCode::kFortsUserorderbookReplInfo:
            case TableCode::kFortsUserorderbookReplOrdersCurrentday:
            case TableCode::kFortsUserorderbookReplMultilegOrdersCurrentday:
            case TableCode::kFortsUserorderbookReplInfoCurrentday:
                return StreamCode::kFortsUserorderbookRepl;
            case TableCode::kFortsPosReplPosition:
            case TableCode::kFortsPosReplPositionSa:
            case TableCode::kFortsPosReplInfo:
                return StreamCode::kFortsPosRepl;
            case TableCode::kFortsPartReplPart:
            case TableCode::kFortsPartReplPartSa:
                return StreamCode::kFortsPartRepl;
            case TableCode::kFortsRefdataReplSession:
            case TableCode::kFortsRefdataReplFutVcb:
            case TableCode::kFortsRefdataReplFutInstruments:
            case TableCode::kFortsRefdataReplFutSessContents:
            case TableCode::kFortsRefdataReplOptSessContents:
            case TableCode::kFortsRefdataReplMultilegDict:
            case TableCode::kFortsRefdataReplInstr2matchingMap:
            case TableCode::kFortsRefdataReplSysMessages:
                return StreamCode::kFortsRefdataRepl;
            case TableCode::kFortsSessionstateReplSessionState:
                return StreamCode::kFortsSessionstateRepl;
            case TableCode::kFortsInstrumentstateReplInstrumentState:
                return StreamCode::kFortsInstrumentstateRepl;
            default:
                return projection::kNoStreamCode;
            }
        };

        const auto stream_code = clear_stream_for_table(table_code);
        auto mark_touched = [&]() {
            if (stream_code == projection::kNoStreamCode) {
                return;
            }
            staged.touched_streams.insert(stream_code);
            const bool regular_userbook_table = stream_code != StreamCode::kFortsUserorderbookRepl ||
                                                is_regular_userorderbook_snapshot_table(table_code);
            if (regular_userbook_table) {
                auto& health = staged.active ? ensure_stream_health(ensure_staged_stream_health(), stream_code)
                                             : ensure_stream_health(stream_health, stream_code);
                reset_stream_watermarks(health);
                if (stream_code == StreamCode::kFortsUserorderbookRepl && staged.active) {
                    staged.userbook_regular_info_seen = false;
                }
            }
        };

        const auto clear_orders = [&](bool trade_source, bool user_source, bool current_day_source) {
            const auto table_it = active_source_revisions().find(table_code);
            if (table_it == active_source_revisions().end()) {
                return;
            }
            for (auto it = orders.begin(); it != orders.end();) {
                const bool owns_requested_surface = (trade_source && it->second.from_trade_repl) ||
                                                    (user_source && it->second.from_user_book) ||
                                                    (current_day_source && it->second.from_current_day);
                if (!owns_requested_surface) {
                    ++it;
                    continue;
                }
                const auto key = revision_key(it->second);
                if (!source_row_is_stale(table_code, key, clear_revision)) {
                    ++it;
                    continue;
                }
                retire_private_history();
                if (trade_source) {
                    it->second.from_trade_repl = false;
                    it->second.trade_repl_commit_sequence = 0;
                }
                if (user_source) {
                    it->second.from_user_book = false;
                    it->second.user_orderbook_commit_sequence = 0;
                }
                if (current_day_source) {
                    it->second.from_current_day = false;
                }
                erase_source_row(table_code, key);
                if (!order_has_any_source(it->second)) {
                    it = orders.erase(it);
                } else {
                    ++it;
                }
            }
        };

        switch (table_code) {
        case TableCode::kFortsTradeReplOrdersLog:
            clear_orders(true, false, false);
            break;
        case TableCode::kFortsTradeReplMultilegOrdersLog:
            clear_orders(true, false, false);
            break;
        case TableCode::kFortsTradeReplUserDeal:
        case TableCode::kFortsTradeReplUserMultilegDeal: {
            for (auto it = trades.begin(); it != trades.end();) {
                const auto key = revision_key(it->second);
                if (source_row_is_stale(table_code, key, clear_revision)) {
                    retire_private_history();
                    erase_source_row(table_code, key);
                    it = trades.erase(it);
                } else {
                    ++it;
                }
            }
            break;
        }
        case TableCode::kFortsUserorderbookReplOrders:
        case TableCode::kFortsUserorderbookReplMultilegOrders:
            clear_orders(false, true, false);
            break;
        case TableCode::kFortsUserorderbookReplOrdersCurrentday:
        case TableCode::kFortsUserorderbookReplMultilegOrdersCurrentday:
            clear_orders(false, false, true);
            break;
        case TableCode::kFortsPosReplPosition:
            for (auto it = positions.begin(); it != positions.end();) {
                const auto key = revision_key(it->first);
                if (source_row_is_stale(table_code, key, clear_revision)) {
                    erase_source_row(table_code, key);
                    it = positions.erase(it);
                } else {
                    ++it;
                }
            }
            break;
        case TableCode::kFortsPartReplPart:
            for (auto it = limits.begin(); it != limits.end();) {
                const auto key = revision_key(it->first);
                if (source_row_is_stale(table_code, key, clear_revision)) {
                    erase_source_row(table_code, key);
                    it = limits.erase(it);
                } else {
                    ++it;
                }
            }
            break;
        case TableCode::kFortsRefdataReplSession:
            for (auto it = sessions.begin(); it != sessions.end();) {
                const auto key = revision_key(it->first);
                if (!source_row_is_stale(table_code, key, clear_revision)) {
                    ++it;
                    continue;
                }
                const auto status_it = active_source_revisions().find(TableCode::kFortsSessionstateReplSessionState);
                const bool keep_status = status_it != active_source_revisions().end() &&
                                         has_source_row(TableCode::kFortsSessionstateReplSessionState, key);
                erase_source_row(table_code, key);
                if (keep_status) {
                    TradingSessionSnapshot replacement{.sess_id = it->first,
                                                       .state = it->second.current_status,
                                                       .has_current_status = it->second.has_current_status,
                                                       .current_status = it->second.current_status};
                    it->second = std::move(replacement);
                    ++it;
                } else {
                    it = sessions.erase(it);
                }
            }
            break;
        case TableCode::kFortsRefdataReplFutSessContents: {
            auto& cache = staged.active
                              ? ensure_stage_copy(staged.future_sessions, future_sessions, native_commit_phase)
                              : future_sessions;
            for (auto it = cache.begin(); it != cache.end();) {
                const auto key = revision_key({std::to_string(it->second.isin_id), std::to_string(it->second.sess_id)});
                if (source_row_is_stale(table_code, key, clear_revision)) {
                    erase_source_row(table_code, key);
                    it = cache.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = instruments.begin(); it != instruments.end();) {
                const auto key = revision_key(it->first);
                if (!it->second.current_session_member ||
                    cache.contains(future_session_key(it->first, it->second.sess_id))) {
                    ++it;
                    continue;
                }
                const bool keep_other = has_source_row(TableCode::kFortsRefdataReplFutInstruments, key) ||
                                        has_source_row(TableCode::kFortsRefdataReplOptSessContents, key) ||
                                        has_source_row(TableCode::kFortsInstrumentstateReplInstrumentState, key);
                if (keep_other) {
                    it->second.sess_id = 0;
                    invalidate_status_bindings();
                    it->second.current_status_refdata_bound = false;
                    it->second.current_session_member = false;
                    it->second.current_session_state = 0;
                    it->second.future_session_terms.reset();
                    ++it;
                } else {
                    it = instruments.erase(it);
                }
            }
            break;
        }
        case TableCode::kFortsRefdataReplFutVcb:
            for (auto it = future_vcb.begin(); it != future_vcb.end();) {
                if (source_row_is_stale(table_code, it->first, clear_revision)) {
                    erase_source_row(table_code, it->first);
                    it = future_vcb.erase(it);
                } else {
                    ++it;
                }
            }
            break;
        case TableCode::kFortsRefdataReplFutInstruments:
        case TableCode::kFortsRefdataReplOptSessContents:
            for (auto it = instruments.begin(); it != instruments.end();) {
                const auto key = revision_key(it->first);
                if (!source_row_is_stale(table_code, key, clear_revision)) {
                    ++it;
                    continue;
                }
                const bool keep_other = has_source_row(table_code == TableCode::kFortsRefdataReplFutInstruments
                                                           ? TableCode::kFortsRefdataReplFutSessContents
                                                           : TableCode::kFortsRefdataReplFutInstruments,
                                                       key) ||
                                        has_source_row(TableCode::kFortsInstrumentstateReplInstrumentState, key);
                erase_source_row(table_code, key);
                if (keep_other) {
                    ++it;
                } else {
                    it = instruments.erase(it);
                }
            }
            break;
        case TableCode::kFortsRefdataReplMultilegDict:
            for (auto& [isin_id, instrument] : instruments) {
                std::erase_if(instrument.legs, [&](const InstrumentLegSnapshot& leg) {
                    const auto key = revision_key({std::to_string(isin_id), std::to_string(leg.leg_order_no)});
                    if (!source_row_is_stale(table_code, key, clear_revision)) {
                        return false;
                    }
                    erase_source_row(table_code, key);
                    return true;
                });
            }
            break;
        case TableCode::kFortsRefdataReplInstr2matchingMap:
            for (auto it = matching.begin(); it != matching.end();) {
                const auto key = revision_key(it->first);
                if (source_row_is_stale(table_code, key, clear_revision)) {
                    erase_source_row(table_code, key);
                    it = matching.erase(it);
                } else {
                    ++it;
                }
            }
            break;
        case TableCode::kFortsRefdataReplSysMessages:
            for (auto it = system_messages.begin(); it != system_messages.end();) {
                const auto key = revision_key(it->first);
                if (source_row_is_stale(table_code, key, clear_revision)) {
                    erase_source_row(table_code, key);
                    it = system_messages.erase(it);
                } else {
                    ++it;
                }
            }
            break;
        case TableCode::kFortsSessionstateReplSessionState:
            for (auto it = sessions.begin(); it != sessions.end();) {
                const auto key = revision_key(it->first);
                if (!source_row_is_stale(table_code, key, clear_revision)) {
                    ++it;
                    continue;
                }
                erase_source_row(table_code, key);
                if (has_source_row(TableCode::kFortsRefdataReplSession, key)) {
                    it->second.has_current_status = false;
                    it->second.current_status = 0;
                    ++it;
                } else {
                    it = sessions.erase(it);
                }
            }
            break;
        case TableCode::kFortsInstrumentstateReplInstrumentState:
            for (auto it = instruments.begin(); it != instruments.end();) {
                const auto key = revision_key(it->first);
                if (!source_row_is_stale(table_code, key, clear_revision)) {
                    ++it;
                    continue;
                }
                erase_source_row(table_code, key);
                if (has_source_row(TableCode::kFortsRefdataReplFutSessContents, key) ||
                    has_source_row(TableCode::kFortsRefdataReplFutInstruments, key) ||
                    has_source_row(TableCode::kFortsRefdataReplOptSessContents, key)) {
                    it->second.has_current_status = false;
                    it->second.current_status = 0;
                    it->second.current_status_refdata_bound = false;
                    ++it;
                } else {
                    it = instruments.erase(it);
                }
            }
            break;
        default:
            break;
        }

        mark_touched();
        if (!staged.active) {
            rebuild_all_snapshots();
            prune_row_changes();
        }
    }

    void clear_stream_owned_state(StreamCode stream_code) {
        switch (stream_code) {
        case StreamCode::kFortsTradeRepl:
            if (!trades_by_key.empty() || std::any_of(orders_by_key.begin(), orders_by_key.end(),
                                                      [](const auto& row) { return row.second.from_trade_repl; }))
                retire_private_history();
            discard_stream_row_changes(stream_code);
            clear_trade_source(orders_by_key);
            trades_by_key.clear();
            rebuild_orders();
            rebuild_trades();
            break;
        case StreamCode::kFortsUserorderbookRepl:
            if (std::any_of(orders_by_key.begin(), orders_by_key.end(),
                            [](const auto& row) { return row.second.from_user_book || row.second.from_current_day; }))
                retire_private_history();
            discard_stream_row_changes(stream_code);
            clear_user_book_source(orders_by_key);
            rebuild_orders();
            break;
        case StreamCode::kFortsPosRepl:
            positions_by_key.clear();
            rebuild_positions();
            break;
        case StreamCode::kFortsPartRepl:
            limits_by_key.clear();
            limit_index.clear();
            unknown_limits = 0;
            rebuild_limits();
            break;
        case StreamCode::kFortsRefdataRepl: {
            future_sessions.clear();
            invalidate_status_bindings();
            // INSTRUMENTSTATE has no session key. A REFDATA generation
            // reset cannot carry its old status into new session membership.
            // Require independently received status again after reconstruction.
            for (auto& [sess_id, session] : sessions_by_id)
                session = TradingSessionSnapshot{.sess_id = sess_id,
                                                 .state = session.current_status,
                                                 .has_current_status = session.has_current_status,
                                                 .current_status = session.current_status};
            std::erase_if(sessions_by_id, [](const auto& item) { return !item.second.has_current_status; });
            for (auto& [isin_id, instrument] : instruments_by_isin)
                instrument = InstrumentSnapshot{.isin_id = isin_id,
                                                .has_current_status = instrument.has_current_status,
                                                .current_status = instrument.current_status};
            std::erase_if(instruments_by_isin, [](const auto& item) { return !item.second.has_current_status; });
        }
            future_vcb_by_row.clear();
            matching_by_base_contract.clear();
            system_messages_by_id.clear();
            rebuild_sessions();
            rebuild_future_vcb();
            rebuild_instruments();
            rebuild_matching_map();
            rebuild_system_messages();
            break;
        case StreamCode::kFortsSessionstateRepl:
            for (auto& [unused_id, session] : sessions_by_id) {
                static_cast<void>(unused_id);
                session.has_current_status = false;
                session.current_status = 0;
            }
            rebuild_sessions();
            break;
        case StreamCode::kFortsInstrumentstateRepl:
            for (auto& [unused_id, instrument] : instruments_by_isin) {
                static_cast<void>(unused_id);
                instrument.has_current_status = false;
                instrument.current_status = 0;
                instrument.current_status_refdata_bound = false;
            }
            rebuild_instruments();
            break;
        default:
            break;
        }

        auto& health = ensure_stream_health(stream_health, stream_code);
        reset_stream_watermarks(health);
    }

    void stage_clear_stream_owned_state(StreamCode stream_code) {
        switch (stream_code) {
        case StreamCode::kFortsTradeRepl: {
            staged.rebuild_order_view = staged.rebuild_trade_view = true;
            auto& orders = ensure_stage_copy(staged.orders, orders_by_key, native_commit_phase);
            auto& trades = ensure_stage_copy(staged.trades, trades_by_key, native_commit_phase);
            if (!trades.empty() ||
                std::any_of(orders.begin(), orders.end(), [](const auto& row) { return row.second.from_trade_repl; }))
                retire_private_history();
            clear_trade_source(orders);
            trades.clear();
            staged.touched_streams.insert(StreamCode::kFortsTradeRepl);
            break;
        }
        case StreamCode::kFortsUserorderbookRepl: {
            staged.rebuild_order_view = true;
            auto& orders = ensure_stage_copy(staged.orders, orders_by_key, native_commit_phase);
            if (std::any_of(orders.begin(), orders.end(),
                            [](const auto& row) { return row.second.from_user_book || row.second.from_current_day; }))
                retire_private_history();
            clear_user_book_source(orders);
            staged.touched_streams.insert(StreamCode::kFortsUserorderbookRepl);
            break;
        }
        case StreamCode::kFortsPosRepl:
            ensure_stage_copy(staged.positions, positions_by_key, native_commit_phase).clear();
            staged.touched_streams.insert(StreamCode::kFortsPosRepl);
            break;
        case StreamCode::kFortsPartRepl:
            ensure_stage_copy(staged.limits, limits_by_key, native_commit_phase).clear();
            staged.touched_streams.insert(StreamCode::kFortsPartRepl);
            break;
        case StreamCode::kFortsRefdataRepl: {
            ensure_stage_copy(staged.future_sessions, future_sessions, native_commit_phase).clear();
            auto& sessions = ensure_stage_copy(staged.sessions, sessions_by_id, native_commit_phase);
            invalidate_status_bindings();
            for (auto& [sess_id, session] : sessions)
                session = TradingSessionSnapshot{.sess_id = sess_id,
                                                 .state = session.current_status,
                                                 .has_current_status = session.has_current_status,
                                                 .current_status = session.current_status};
            std::erase_if(sessions, [](const auto& item) { return !item.second.has_current_status; });
            auto& instruments = ensure_stage_copy(staged.instruments, instruments_by_isin, native_commit_phase);
            for (auto& [isin_id, instrument] : instruments)
                instrument = InstrumentSnapshot{.isin_id = isin_id,
                                                .has_current_status = instrument.has_current_status,
                                                .current_status = instrument.current_status};
            std::erase_if(instruments, [](const auto& item) { return !item.second.has_current_status; });
            ensure_stage_copy(staged.future_vcb, future_vcb_by_row, native_commit_phase).clear();
        }
            ensure_stage_copy(staged.matching_map, matching_by_base_contract, native_commit_phase).clear();
            ensure_stage_copy(staged.system_messages, system_messages_by_id, native_commit_phase).clear();
            staged.touched_streams.insert(StreamCode::kFortsRefdataRepl);
            break;
        case StreamCode::kFortsSessionstateRepl:
            for (auto& [unused_id, session] : ensure_stage_copy(staged.sessions, sessions_by_id, native_commit_phase)) {
                static_cast<void>(unused_id);
                session.has_current_status = false;
                session.current_status = 0;
            }
            staged.touched_streams.insert(StreamCode::kFortsSessionstateRepl);
            break;
        case StreamCode::kFortsInstrumentstateRepl:
            for (auto& [unused_id, instrument] :
                 ensure_stage_copy(staged.instruments, instruments_by_isin, native_commit_phase)) {
                static_cast<void>(unused_id);
                instrument.has_current_status = false;
                instrument.current_status = 0;
                instrument.current_status_refdata_bound = false;
            }
            staged.touched_streams.insert(StreamCode::kFortsInstrumentstateRepl);
            break;
        default:
            break;
        }

        auto& health = ensure_stream_health(ensure_staged_stream_health(), stream_code);
        reset_stream_watermarks(health);
    }

    OrderMap& ensure_staged_orders(StreamCode stream_code) {
        staged.touched_streams.insert(stream_code);
        return ensure_stage_copy(staged.orders, orders_by_key, native_commit_phase);
    }

    SessionMap& ensure_staged_sessions() {
        staged.touched_streams.insert(StreamCode::kFortsRefdataRepl);
        return ensure_stage_copy(staged.sessions, sessions_by_id, native_commit_phase);
    }

    InstrumentMap& ensure_staged_instruments() {
        staged.touched_streams.insert(StreamCode::kFortsRefdataRepl);
        return ensure_stage_copy(staged.instruments, instruments_by_isin, native_commit_phase);
    }

    FutureVcbMap& ensure_staged_future_vcb() {
        staged.touched_streams.insert(StreamCode::kFortsRefdataRepl);
        return ensure_stage_copy(staged.future_vcb, future_vcb_by_row, native_commit_phase);
    }

    MatchingMap& ensure_staged_matching_map() {
        staged.touched_streams.insert(StreamCode::kFortsRefdataRepl);
        return ensure_stage_copy(staged.matching_map, matching_by_base_contract, native_commit_phase);
    }

    SystemMessageMap& ensure_staged_system_messages() {
        staged.touched_streams.insert(StreamCode::kFortsRefdataRepl);
        return ensure_stage_copy(staged.system_messages, system_messages_by_id, native_commit_phase);
    }

    LimitMap& ensure_staged_limits() {
        staged.touched_streams.insert(StreamCode::kFortsPartRepl);
        return ensure_stage_copy(staged.limits, limits_by_key, native_commit_phase);
    }

    PositionMap& ensure_staged_positions() {
        staged.touched_streams.insert(StreamCode::kFortsPosRepl);
        return ensure_stage_copy(staged.positions, positions_by_key, native_commit_phase);
    }

    TradeMap& ensure_staged_trades() {
        staged.touched_streams.insert(StreamCode::kFortsTradeRepl);
        return ensure_stage_copy(staged.trades, trades_by_key, native_commit_phase);
    }

    void record_order_change(const OwnOrderSnapshot& order, OrderKey key) {
        if (!order_change_pending(order))
            staged.order_keys.push_back(std::move(key));
    }

    void apply_trade_order_row(const RowReader& row, bool multileg) {
        auto& orders = ensure_staged_orders(StreamCode::kFortsTradeRepl);
        OrderKey key{
            .surface = OrderSurface::kTrade,
            .multileg = multileg,
            .public_order_id = row.i64(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogPublicOrderId
                                                : FieldCode::kFortsTradeReplOrdersLogPublicOrderId),
            .private_order_id = row.i64(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogPrivateOrderId
                                                 : FieldCode::kFortsTradeReplOrdersLogPrivateOrderId),
            .ext_id = row.i32(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogExtId
                                       : FieldCode::kFortsTradeReplOrdersLogExtId),
            .client_code = row.text(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogClientCode
                                             : FieldCode::kFortsTradeReplOrdersLogClientCode),
        };
        const auto previous_id = row.i64(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogIdOrd1
                                                  : FieldCode::kFortsTradeReplOrdersLogIdOrd1);
        OrderKey resolved;
        // Native TRADE rows are exchange records. Move can reuse ext_id for a
        // distinct positive order ID before reply 176 supplies the logical link.
        auto& order = find_or_create_order(orders, key, &resolved, true, !native_commit_phase && previous_id == 0,
                                           !native_commit_phase);
        record_order_change(order, std::move(resolved));
        order.sess_id = row.i32(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogSessId
                                         : FieldCode::kFortsTradeReplOrdersLogSessId);
        order.isin_id = row.i32(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogIsinId
                                         : FieldCode::kFortsTradeReplOrdersLogIsinId);
        order.login_from = row.text(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogLoginFrom
                                             : FieldCode::kFortsTradeReplOrdersLogLoginFrom);
        order.comment = row.text(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogComment
                                          : FieldCode::kFortsTradeReplOrdersLogComment);
        order.price = row.text(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogPrice
                                        : FieldCode::kFortsTradeReplOrdersLogPrice);
        order.public_amount = row.i64(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogPublicAmount
                                               : FieldCode::kFortsTradeReplOrdersLogPublicAmount);
        order.public_amount_rest = row.i64(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogPublicAmountRest
                                                    : FieldCode::kFortsTradeReplOrdersLogPublicAmountRest);
        order.private_amount = row.i64(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogPrivateAmount
                                                : FieldCode::kFortsTradeReplOrdersLogPrivateAmount);
        order.private_amount_rest = row.i64(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogPrivateAmountRest
                                                     : FieldCode::kFortsTradeReplOrdersLogPrivateAmountRest);
        order.id_deal = row.i64(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogIdDeal
                                         : FieldCode::kFortsTradeReplOrdersLogIdDeal);
        order.id_ord1 = previous_id;
        order.xstatus = row.i64(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogXstatus
                                         : FieldCode::kFortsTradeReplOrdersLogXstatus);
        order.xstatus2 = row.i64(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogXstatus2
                                          : FieldCode::kFortsTradeReplOrdersLogXstatus2);
        order.dir =
            row.i8(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogDir : FieldCode::kFortsTradeReplOrdersLogDir);
        order.public_action = row.i8(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogPublicAction
                                              : FieldCode::kFortsTradeReplOrdersLogPublicAction);
        order.private_action = row.i8(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogPrivateAction
                                               : FieldCode::kFortsTradeReplOrdersLogPrivateAction);
        order.moment = row.i64(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogMoment
                                        : FieldCode::kFortsTradeReplOrdersLogMoment);
        order.moment_ns = row.u64(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogMomentNs
                                           : FieldCode::kFortsTradeReplOrdersLogMomentNs);
        if (!order.moment_ns)
            order.moment_ns = row.timestamp_ns(multileg ? FieldCode::kFortsTradeReplMultilegOrdersLogMoment
                                                        : FieldCode::kFortsTradeReplOrdersLogMoment);
        order.from_trade_repl = true;
        order.trade_repl_commit_sequence = std::numeric_limits<std::uint64_t>::max();
    }

    void apply_user_book_order_row(const RowReader& row, bool multileg, bool current_day) {
        auto& orders = ensure_staged_orders(StreamCode::kFortsUserorderbookRepl);
        const auto public_order_id_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayPublicOrderId
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersPublicOrderId)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicOrderId
                                    : FieldCode::kFortsUserorderbookReplOrdersPublicOrderId);
        const auto private_order_id_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayPrivateOrderId
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersPrivateOrderId)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateOrderId
                                    : FieldCode::kFortsUserorderbookReplOrdersPrivateOrderId);
        const auto ext_id_field = multileg
                                      ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayExtId
                                                     : FieldCode::kFortsUserorderbookReplMultilegOrdersExtId)
                                      : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayExtId
                                                     : FieldCode::kFortsUserorderbookReplOrdersExtId);
        const auto client_code_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayClientCode
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersClientCode)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayClientCode
                                    : FieldCode::kFortsUserorderbookReplOrdersClientCode);
        const auto sess_field = multileg
                                    ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdaySessId
                                                   : FieldCode::kFortsUserorderbookReplMultilegOrdersSessId)
                                    : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdaySessId
                                                   : FieldCode::kFortsUserorderbookReplOrdersSessId);
        const auto isin_field = multileg
                                    ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayIsinId
                                                   : FieldCode::kFortsUserorderbookReplMultilegOrdersIsinId)
                                    : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayIsinId
                                                   : FieldCode::kFortsUserorderbookReplOrdersIsinId);
        const auto login_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayLoginFrom
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersLoginFrom)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayLoginFrom
                                    : FieldCode::kFortsUserorderbookReplOrdersLoginFrom);
        const auto comment_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayComment
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersComment)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayComment
                                    : FieldCode::kFortsUserorderbookReplOrdersComment);
        const auto price_field = multileg
                                     ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayPrice
                                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersPrice)
                                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrice
                                                    : FieldCode::kFortsUserorderbookReplOrdersPrice);
        const auto public_amount_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayPublicAmount
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersPublicAmount)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicAmount
                                    : FieldCode::kFortsUserorderbookReplOrdersPublicAmount);
        const auto public_rest_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayPublicAmountRest
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersPublicAmountRest)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicAmountRest
                                    : FieldCode::kFortsUserorderbookReplOrdersPublicAmountRest);
        const auto private_amount_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayPrivateAmount
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersPrivateAmount)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateAmount
                                    : FieldCode::kFortsUserorderbookReplOrdersPrivateAmount);
        const auto private_rest_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayPrivateAmountRest
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersPrivateAmountRest)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateAmountRest
                                    : FieldCode::kFortsUserorderbookReplOrdersPrivateAmountRest);
        const auto xstatus_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayXstatus
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersXstatus)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayXstatus
                                    : FieldCode::kFortsUserorderbookReplOrdersXstatus);
        const auto xstatus2_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayXstatus2
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersXstatus2)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayXstatus2
                                    : FieldCode::kFortsUserorderbookReplOrdersXstatus2);
        const auto dir_field = multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayDir
                                                       : FieldCode::kFortsUserorderbookReplMultilegOrdersDir)
                                        : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayDir
                                                       : FieldCode::kFortsUserorderbookReplOrdersDir);
        const auto public_action_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayPublicAction
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersPublicAction)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayPublicAction
                                    : FieldCode::kFortsUserorderbookReplOrdersPublicAction);
        const auto private_action_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayPrivateAction
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersPrivateAction)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayPrivateAction
                                    : FieldCode::kFortsUserorderbookReplOrdersPrivateAction);
        const auto moment_field = multileg
                                      ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayMoment
                                                     : FieldCode::kFortsUserorderbookReplMultilegOrdersMoment)
                                      : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayMoment
                                                     : FieldCode::kFortsUserorderbookReplOrdersMoment);
        const auto moment_ns_field =
            multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayMomentNs
                                    : FieldCode::kFortsUserorderbookReplMultilegOrdersMomentNs)
                     : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayMomentNs
                                    : FieldCode::kFortsUserorderbookReplOrdersMomentNs);

        OrderKey key{
            .surface = OrderSurface::kUserOrderbook,
            .multileg = multileg,
            .public_order_id = row.i64(public_order_id_field),
            .private_order_id = row.i64(private_order_id_field),
            .ext_id = row.i32(ext_id_field),
            .client_code = row.text(client_code_field),
        };
        OrderKey resolved;
        auto& order = find_or_create_order(orders, key, &resolved, true, !native_commit_phase, !native_commit_phase);
        record_order_change(order, std::move(resolved));

        order.sess_id = row.i32(sess_field);
        order.isin_id = row.i32(isin_field);
        order.login_from = row.text(login_field);
        order.comment = row.text(comment_field);
        order.id_ord1 =
            row.i64(multileg ? (current_day ? FieldCode::kFortsUserorderbookReplMultilegOrdersCurrentdayIdOrd1
                                            : FieldCode::kFortsUserorderbookReplMultilegOrdersIdOrd1)
                             : (current_day ? FieldCode::kFortsUserorderbookReplOrdersCurrentdayIdOrd1
                                            : FieldCode::kFortsUserorderbookReplOrdersIdOrd1));
        order.price = row.text(price_field);
        order.public_amount = row.i64(public_amount_field);
        order.public_amount_rest = row.i64(public_rest_field);
        order.private_amount = row.i64(private_amount_field);
        order.private_amount_rest = row.i64(private_rest_field);
        order.xstatus = row.i64(xstatus_field);
        order.xstatus2 = row.i64(xstatus2_field);
        order.dir = row.i8(dir_field);
        order.public_action = row.i8(public_action_field);
        order.private_action = row.i8(private_action_field);
        order.moment = row.i64(moment_field);
        order.moment_ns = row.u64(moment_ns_field);
        if (!order.moment_ns)
            order.moment_ns = row.timestamp_ns(moment_field);
        order.from_user_book = !current_day;
        order.from_current_day = current_day;
        order.user_orderbook_commit_sequence = std::numeric_limits<std::uint64_t>::max();
    }

    void apply_trade_row(const RowReader& row, bool multileg) {
        auto& trades = ensure_staged_trades();
        const auto key = TradeKey{
            .multileg = multileg,
            .id_deal = row.i64(multileg ? FieldCode::kFortsTradeReplUserMultilegDealIdDeal
                                        : FieldCode::kFortsTradeReplUserDealIdDeal),
        };
        auto& trade = trades[key];
        staged.trade_keys.push_back(key);
        trade.multileg = multileg;
        trade.id_deal = key.id_deal;
        trade.sess_id = row.i32(multileg ? FieldCode::kFortsTradeReplUserMultilegDealSessId
                                         : FieldCode::kFortsTradeReplUserDealSessId);
        trade.isin_id = row.i32(multileg ? FieldCode::kFortsTradeReplUserMultilegDealIsinId
                                         : FieldCode::kFortsTradeReplUserDealIsinId);
        trade.price = row.text(multileg ? FieldCode::kFortsTradeReplUserMultilegDealPrice
                                        : FieldCode::kFortsTradeReplUserDealPrice);
        if (multileg) {
            trade.rate_price = row.text(FieldCode::kFortsTradeReplUserMultilegDealRatePrice);
            trade.swap_price = row.text(FieldCode::kFortsTradeReplUserMultilegDealSwapPrice);
        }
        trade.amount = row.i64(multileg ? FieldCode::kFortsTradeReplUserMultilegDealXamount
                                        : FieldCode::kFortsTradeReplUserDealXamount);
        trade.public_order_id_buy = row.i64(multileg ? FieldCode::kFortsTradeReplUserMultilegDealPublicOrderIdBuy
                                                     : FieldCode::kFortsTradeReplUserDealPublicOrderIdBuy);
        trade.public_order_id_sell = row.i64(multileg ? FieldCode::kFortsTradeReplUserMultilegDealPublicOrderIdSell
                                                      : FieldCode::kFortsTradeReplUserDealPublicOrderIdSell);
        trade.private_order_id_buy = row.i64(multileg ? FieldCode::kFortsTradeReplUserMultilegDealPrivateOrderIdBuy
                                                      : FieldCode::kFortsTradeReplUserDealPrivateOrderIdBuy);
        trade.private_order_id_sell = row.i64(multileg ? FieldCode::kFortsTradeReplUserMultilegDealPrivateOrderIdSell
                                                       : FieldCode::kFortsTradeReplUserDealPrivateOrderIdSell);
        trade.ext_id_buy = row.i32(multileg ? FieldCode::kFortsTradeReplUserMultilegDealExtIdBuy
                                            : FieldCode::kFortsTradeReplUserDealExtIdBuy);
        trade.ext_id_sell = row.i32(multileg ? FieldCode::kFortsTradeReplUserMultilegDealExtIdSell
                                             : FieldCode::kFortsTradeReplUserDealExtIdSell);
        trade.code_buy = row.text(multileg ? FieldCode::kFortsTradeReplUserMultilegDealCodeBuy
                                           : FieldCode::kFortsTradeReplUserDealCodeBuy);
        trade.code_sell = row.text(multileg ? FieldCode::kFortsTradeReplUserMultilegDealCodeSell
                                            : FieldCode::kFortsTradeReplUserDealCodeSell);
        trade.comment_buy = row.text(multileg ? FieldCode::kFortsTradeReplUserMultilegDealCommentBuy
                                              : FieldCode::kFortsTradeReplUserDealCommentBuy);
        trade.comment_sell = row.text(multileg ? FieldCode::kFortsTradeReplUserMultilegDealCommentSell
                                               : FieldCode::kFortsTradeReplUserDealCommentSell);
        trade.login_buy = row.text(multileg ? FieldCode::kFortsTradeReplUserMultilegDealLoginBuy
                                            : FieldCode::kFortsTradeReplUserDealLoginBuy);
        trade.login_sell = row.text(multileg ? FieldCode::kFortsTradeReplUserMultilegDealLoginSell
                                             : FieldCode::kFortsTradeReplUserDealLoginSell);
        trade.moment = row.i64(multileg ? FieldCode::kFortsTradeReplUserMultilegDealMoment
                                        : FieldCode::kFortsTradeReplUserDealMoment);
        trade.moment_ns = row.u64(multileg ? FieldCode::kFortsTradeReplUserMultilegDealMomentNs
                                           : FieldCode::kFortsTradeReplUserDealMomentNs);
        if (!trade.moment_ns)
            trade.moment_ns = row.timestamp_ns(multileg ? FieldCode::kFortsTradeReplUserMultilegDealMoment
                                                        : FieldCode::kFortsTradeReplUserDealMoment);
    }

    void apply_position_row(const RowReader& row) {
        auto& positions = ensure_staged_positions();
        PositionKey key{
            .scope = PositionScope::kClient,
            .account_code = row.text(FieldCode::kFortsPosReplPositionClientCode),
            .isin_id = row.i32(FieldCode::kFortsPosReplPositionIsinId),
            .account_type = row.i8(FieldCode::kFortsPosReplPositionAccountType),
        };
        auto& position = positions[key];
        position.scope = PositionScope::kClient;
        position.account_code = key.account_code;
        position.isin_id = key.isin_id;
        position.account_type = key.account_type;
        position.xpos = row.i64(FieldCode::kFortsPosReplPositionXpos);
        position.xbuys_qty = row.i64(FieldCode::kFortsPosReplPositionXbuysQty);
        position.xsells_qty = row.i64(FieldCode::kFortsPosReplPositionXsellsQty);
        position.xday_open_qty = row.i64(FieldCode::kFortsPosReplPositionXdayOpenQty);
        position.xday_open_buys_qty = row.i64(FieldCode::kFortsPosReplPositionXdayOpenBuysQty);
        position.xday_open_sells_qty = row.i64(FieldCode::kFortsPosReplPositionXdayOpenSellsQty);
        position.xopen_qty = row.i64(FieldCode::kFortsPosReplPositionXopenQty);
        position.waprice = row.text(FieldCode::kFortsPosReplPositionWaprice);
        position.net_volume_rur = row.text(FieldCode::kFortsPosReplPositionNetVolumeRur);
        position.last_deal_id = row.i64(FieldCode::kFortsPosReplPositionLastDealId);
        position.last_quantity = row.i64(FieldCode::kFortsPosReplPositionLastQuantity);
    }

    void apply_limit_row(const RowReader& row) {
        auto& limits = ensure_staged_limits();
        LimitKey key{
            .participant_kind = classify_limit_participant(row.text(FieldCode::kFortsPartReplPartClientCode)),
            .account_code = row.text(FieldCode::kFortsPartReplPartClientCode),
            .repl_id = row.i64(FieldCode::kFortsPartReplPartReplId),
        };
        auto& limit = limits[key];
        limit.participant_kind = key.participant_kind;
        limit.repl_id = key.repl_id;
        limit.account_code = key.account_code;
        limit.limits_set = row.boolean(FieldCode::kFortsPartReplPartLimitsSet);
        limit.is_auto_update_limit = row.boolean(FieldCode::kFortsPartReplPartIsAutoUpdateLimit);
        limit.money_free = row.text(FieldCode::kFortsPartReplPartMoneyFree);
        limit.money_blocked = row.text(FieldCode::kFortsPartReplPartMoneyBlocked);
        limit.vm_reserve = row.text(FieldCode::kFortsPartReplPartVmReserve);
        limit.fee = row.text(FieldCode::kFortsPartReplPartFee);
        limit.money_old = row.text(FieldCode::kFortsPartReplPartMoneyOld);
        limit.money_amount = row.text(FieldCode::kFortsPartReplPartMoneyAmount);
        limit.money_pledge_amount = row.text(FieldCode::kFortsPartReplPartMoneyPledgeAmount);
        limit.actual_amount_of_base_currency = row.text(FieldCode::kFortsPartReplPartActualAmountOfBaseCurrency);
        limit.vm_intercl = row.text(FieldCode::kFortsPartReplPartVmIntercl);
        limit.broker_fee = row.text(FieldCode::kFortsPartReplPartBrokerFee);
        limit.penalty = row.text(FieldCode::kFortsPartReplPartPenalty);
        limit.premium_intercl = row.text(FieldCode::kFortsPartReplPartPremiumIntercl);
        limit.net_option_value = row.text(FieldCode::kFortsPartReplPartNetOptionValue);
    }

    void apply_session_row(const RowReader& row) {
        auto& sessions = ensure_staged_sessions();
        const auto sess_id = row.i32(FieldCode::kFortsRefdataReplSessionSessId);
        auto& session = sessions[sess_id];
        session.sess_id = sess_id;
        session.begin = row.i64(FieldCode::kFortsRefdataReplSessionBegin);
        session.end = row.i64(FieldCode::kFortsRefdataReplSessionEnd);
        session.state = row.i32(FieldCode::kFortsRefdataReplSessionState);
        session.inter_cl_begin = row.i64(FieldCode::kFortsRefdataReplSessionInterClBegin);
        session.inter_cl_end = row.i64(FieldCode::kFortsRefdataReplSessionInterClEnd);
        session.inter_cl_state = row.i32(FieldCode::kFortsRefdataReplSessionInterClState);
        session.eve_on = row.boolean(FieldCode::kFortsRefdataReplSessionEveOn);
        session.eve_begin = row.i64(FieldCode::kFortsRefdataReplSessionEveBegin);
        session.eve_end = row.i64(FieldCode::kFortsRefdataReplSessionEveEnd);
        session.mon_on = row.boolean(FieldCode::kFortsRefdataReplSessionMonOn);
        session.mon_begin = row.i64(FieldCode::kFortsRefdataReplSessionMonBegin);
        session.mon_end = row.i64(FieldCode::kFortsRefdataReplSessionMonEnd);
        session.settl_sess_begin = row.i64(FieldCode::kFortsRefdataReplSessionSettlSessBegin);
        session.clr_sess_begin = row.i64(FieldCode::kFortsRefdataReplSessionClrSessBegin);
        session.settl_price_calc_time = row.i64(FieldCode::kFortsRefdataReplSessionSettlPriceCalcTime);
        session.settl_sess_t1_begin = row.i64(FieldCode::kFortsRefdataReplSessionSettlSessT1Begin);
        session.margin_call_fix_schedule = row.i64(FieldCode::kFortsRefdataReplSessionMarginCallFixSchedule);
    }

    void apply_future_instrument_row(const projection::EventSpec& event, const RowReader& row) {
        auto& instruments = ensure_staged_instruments();
        const auto isin_id = row.i32(FieldCode::kFortsRefdataReplFutInstrumentsIsinId);
        auto& instrument = instruments[isin_id];
        instrument.isin_id = isin_id;
        instrument.kind = InstrumentKind::kFuture;
        instrument.isin = row.text(FieldCode::kFortsRefdataReplFutInstrumentsIsin);
        instrument.short_isin = row.text(FieldCode::kFortsRefdataReplFutInstrumentsShortIsin);
        instrument.name = row.text(FieldCode::kFortsRefdataReplFutInstrumentsName);
        instrument.base_contract_code = row.text(FieldCode::kFortsRefdataReplFutInstrumentsBaseContractCode);
        instrument.inst_term = row.i32(FieldCode::kFortsRefdataReplFutInstrumentsInstTerm);
        instrument.roundto = row.i32(FieldCode::kFortsRefdataReplFutInstrumentsRoundto);
        instrument.lot_volume = row.i32(FieldCode::kFortsRefdataReplFutInstrumentsLotVolume);
        instrument.trade_mode_id = row.i32(FieldCode::kFortsRefdataReplFutInstrumentsTradeModeId);
        instrument.signs = row.i32(FieldCode::kFortsRefdataReplFutInstrumentsSigns);
        instrument.is_spread = row.boolean(FieldCode::kFortsRefdataReplFutInstrumentsIsSpread);
        instrument.min_step = row.text(FieldCode::kFortsRefdataReplFutInstrumentsMinStep);
        instrument.step_price = row.text(FieldCode::kFortsRefdataReplFutInstrumentsStepPrice);
        instrument.step_price_curr = row.text(FieldCode::kFortsRefdataReplFutInstrumentsStepPriceCurr);
        instrument.settlement_price = row.text(FieldCode::kFortsRefdataReplFutInstrumentsSettlementPrice);
        instrument.last_trade_date = row.i64(FieldCode::kFortsRefdataReplFutInstrumentsLastTradeDate);
        instrument.group_mask = row.i64(FieldCode::kFortsRefdataReplFutInstrumentsGroupMask);
        instrument.trade_period_access = row.i64(FieldCode::kFortsRefdataReplFutInstrumentsTradePeriodAccess);
        const auto lifenum = refdata_lifenum();
        instrument.definition_source_provenance = {.stream_code = StreamCode::kFortsRefdataRepl,
                                                   .table_code = TableCode::kFortsRefdataReplFutInstruments,
                                                   .repl_rev = event.signed_value,
                                                   .lifenum = lifenum.value_or(0),
                                                   .present = lifenum.has_value()};
    }

    void apply_future_vcb_row(const projection::EventSpec& event, const RowReader& row) {
        auto& rows = ensure_staged_future_vcb();
        const auto key = future_vcb_row_key(row);
        auto& vcb = rows[key];
        vcb.repl_id = row.i64(FieldCode::kFortsRefdataReplFutVcbReplId);
        vcb.base_contract_id = row.i32(FieldCode::kFortsRefdataReplFutVcbBaseContractId);
        vcb.base_contract_code = row.text(FieldCode::kFortsRefdataReplFutVcbBaseContractCode);
        // These are the locked schema fields: curr is quotation currency and
        // board_md is the ASTS SECBOARD identifier. Preserve raw text.
        vcb.currency = row.text(FieldCode::kFortsRefdataReplFutVcbCurr);
        vcb.board_md = row.text(FieldCode::kFortsRefdataReplFutVcbBoardMd);
        vcb.source = {.stream_code = StreamCode::kFortsRefdataRepl,
                      .table_code = TableCode::kFortsRefdataReplFutVcb,
                      .repl_rev = event.signed_value,
                      .lifenum = refdata_lifenum().value_or(0),
                      .present = refdata_lifenum().has_value()};
    }

    void apply_future_session_contents_row(const projection::EventSpec& event, const RowReader& row) {
        auto& instruments = ensure_staged_instruments();
        staged.touched_streams.insert(StreamCode::kFortsRefdataRepl);
        const auto isin_id = row.i32(FieldCode::kFortsRefdataReplFutSessContentsIsinId);
        auto instrument = instruments[isin_id];
        instrument.isin_id = isin_id;
        const auto sess_id = row.i32(FieldCode::kFortsRefdataReplFutSessContentsSessId);
        if (instrument.sess_id != sess_id || !instrument.current_session_member) {
            // Also handles status-before-membership at initial startup. Only
            // a subsequent independent status row can re-establish currentness.
            instrument.current_status_refdata_bound = false;
        }
        instrument.sess_id = sess_id;
        instrument.kind = InstrumentKind::kFuture;
        instrument.isin = row.text(FieldCode::kFortsRefdataReplFutSessContentsIsin);
        instrument.short_isin = row.text(FieldCode::kFortsRefdataReplFutSessContentsShortIsin);
        instrument.name = row.text(FieldCode::kFortsRefdataReplFutSessContentsName);
        instrument.base_contract_code = row.text(FieldCode::kFortsRefdataReplFutSessContentsBaseContractCode);
        instrument.inst_term = row.i32(FieldCode::kFortsRefdataReplFutSessContentsInstTerm);
        instrument.roundto = row.i32(FieldCode::kFortsRefdataReplFutSessContentsRoundto);
        instrument.lot_volume = row.i32(FieldCode::kFortsRefdataReplFutSessContentsLotVolume);
        instrument.trade_mode_id = row.i32(FieldCode::kFortsRefdataReplFutSessContentsTradeModeId);
        instrument.state = row.i32(FieldCode::kFortsRefdataReplFutSessContentsState);
        instrument.signs = row.i32(FieldCode::kFortsRefdataReplFutSessContentsSigns);
        instrument.is_spread = row.boolean(FieldCode::kFortsRefdataReplFutSessContentsIsSpread);
        instrument.min_step = row.text(FieldCode::kFortsRefdataReplFutSessContentsMinStep);
        instrument.step_price = row.text(FieldCode::kFortsRefdataReplFutSessContentsStepPrice);
        instrument.step_price_curr = row.text(FieldCode::kFortsRefdataReplFutSessContentsStepPriceCurr);
        instrument.settlement_price = row.text(FieldCode::kFortsRefdataReplFutSessContentsSettlementPrice);
        const auto lifenum = refdata_lifenum();
        instrument.definition_source_provenance = {.stream_code = StreamCode::kFortsRefdataRepl,
                                                   .table_code = TableCode::kFortsRefdataReplFutSessContents,
                                                   .repl_rev = event.signed_value,
                                                   .lifenum = lifenum.value_or(0),
                                                   .present = lifenum.has_value()};
        instrument.last_trade_date = row.i64(FieldCode::kFortsRefdataReplFutSessContentsLastTradeDate);
        instrument.group_mask = row.i64(FieldCode::kFortsRefdataReplFutSessContentsGroupMask);
        instrument.trade_period_access = row.i64(FieldCode::kFortsRefdataReplFutSessContentsTradePeriodAccess);
        instrument.current_session_member = row.i32(FieldCode::kFortsRefdataReplFutSessContentsReplAct) == 0;
        instrument.current_session_state = instrument.state;
        instrument.future_session_terms.reset();
        if (instrument.current_session_member) {
            const auto decimal = [&](FieldCode code) -> std::optional<SessionDecimal> {
                const auto* field = row.find(code);
                if (!field ||
                    (field->kind != projection::ValueKind::kDecimal && field->kind != projection::ValueKind::kString))
                    return std::nullopt;
                return parse_session_decimal(field->text_value);
            };
            FutureSessionTerms terms{
                .isin_id = isin_id,
                .sess_id = instrument.sess_id,
                .settlement_price = decimal(FieldCode::kFortsRefdataReplFutSessContentsSettlementPrice),
                .raw_limit_up = decimal(FieldCode::kFortsRefdataReplFutSessContentsLimitUp),
                .raw_limit_down = decimal(FieldCode::kFortsRefdataReplFutSessContentsLimitDown),
                .min_step = decimal(FieldCode::kFortsRefdataReplFutSessContentsMinStep),
                .repl_id = row.i64(FieldCode::kFortsRefdataReplFutSessContentsReplId),
                .source = {.stream_code = StreamCode::kFortsRefdataRepl,
                           .table_code = TableCode::kFortsRefdataReplFutSessContents,
                           .repl_rev = event.signed_value,
                           .lifenum = refdata_lifenum().value_or(0),
                           .present = refdata_lifenum().has_value()},
            };
            terms.bounds = evaluate_future_price_bounds(terms);
            instrument.future_session_terms = terms;
        }
        auto& cache = ensure_stage_copy(staged.future_sessions, future_sessions, native_commit_phase);
        cache[future_session_key(isin_id, sess_id)] = instrument;
        const auto current = select_session(session_snapshots);
        if (!current || current == sess_id || instruments[isin_id].sess_id == 0) {
            if (instruments[isin_id].sess_id != sess_id || !instruments[isin_id].current_session_member)
                invalidate_status_bindings();
            instruments[isin_id] = std::move(instrument);
        }
    }

    void apply_session_status_row(const RowReader& row) {
        auto& sessions = ensure_staged_sessions();
        staged.touched_streams.insert(StreamCode::kFortsSessionstateRepl);
        const auto sess_id = row.i32(FieldCode::kFortsSessionstateReplSessionStateSessId);
        auto& session = sessions[sess_id];
        session.sess_id = sess_id;
        session.has_current_status = true;
        session.current_status = row.i32(FieldCode::kFortsSessionstateReplSessionStatePublicState);
        if (session.state == 0) {
            session.state = session.current_status;
        }
    }

    void apply_instrument_status_row(const RowReader& row) {
        auto& instruments = ensure_staged_instruments();
        staged.touched_streams.insert(StreamCode::kFortsInstrumentstateRepl);
        const auto isin_id = row.i32(FieldCode::kFortsInstrumentstateReplInstrumentStateIsinId);
        auto& instrument = instruments[isin_id];
        instrument.isin_id = isin_id;
        instrument.has_current_status = true;
        instrument.current_status = row.i32(FieldCode::kFortsInstrumentstateReplInstrumentStatePublicState);
        instrument.current_status_refdata_bound = instrument.sess_id > 0 && instrument.current_session_member;
    }

    void apply_option_instrument_row(const RowReader& row) {
        auto& instruments = ensure_staged_instruments();
        const auto isin_id = row.i32(FieldCode::kFortsRefdataReplOptSessContentsIsinId);
        auto& instrument = instruments[isin_id];
        instrument.isin_id = isin_id;
        instrument.sess_id = row.i32(FieldCode::kFortsRefdataReplOptSessContentsSessId);
        instrument.kind = InstrumentKind::kOption;
        instrument.isin = row.text(FieldCode::kFortsRefdataReplOptSessContentsIsin);
        instrument.short_isin = row.text(FieldCode::kFortsRefdataReplOptSessContentsShortIsin);
        instrument.name = row.text(FieldCode::kFortsRefdataReplOptSessContentsName);
        instrument.base_contract_code = row.text(FieldCode::kFortsRefdataReplOptSessContentsBaseContractCode);
        instrument.fut_isin_id = row.i32(FieldCode::kFortsRefdataReplOptSessContentsFutIsinId);
        instrument.option_series_id = row.i32(FieldCode::kFortsRefdataReplOptSessContentsOptionSeriesId);
        instrument.roundto = row.i32(FieldCode::kFortsRefdataReplOptSessContentsRoundto);
        instrument.trade_mode_id = row.i32(FieldCode::kFortsRefdataReplOptSessContentsTradeModeId);
        instrument.state = row.i32(FieldCode::kFortsRefdataReplOptSessContentsState);
        instrument.signs = row.i32(FieldCode::kFortsRefdataReplOptSessContentsSigns);
        instrument.put = row.boolean(FieldCode::kFortsRefdataReplOptSessContentsPut);
        instrument.strike = row.text(FieldCode::kFortsRefdataReplOptSessContentsStrike);
        instrument.settlement_price = row.text(FieldCode::kFortsRefdataReplOptSessContentsSettlementPrice);
        instrument.last_trade_date = row.i64(FieldCode::kFortsRefdataReplOptSessContentsLastTradeDate);
        instrument.group_mask = row.i64(FieldCode::kFortsRefdataReplOptSessContentsGroupMask);
        instrument.trade_period_access = row.i64(FieldCode::kFortsRefdataReplOptSessContentsTradePeriodAccess);
    }

    void apply_multileg_leg_row(const RowReader& row) {
        auto& instruments = ensure_staged_instruments();
        const auto isin_id = row.i32(FieldCode::kFortsRefdataReplMultilegDictIsinId);
        auto& instrument = instruments[isin_id];
        instrument.isin_id = isin_id;
        instrument.sess_id = row.i32(FieldCode::kFortsRefdataReplMultilegDictSessId);
        instrument.kind = InstrumentKind::kMultileg;
        append_or_replace_leg(instrument.legs,
                              {
                                  .leg_isin_id = row.i32(FieldCode::kFortsRefdataReplMultilegDictIsinIdLeg),
                                  .qty_ratio = row.i32(FieldCode::kFortsRefdataReplMultilegDictQtyRatio),
                                  .leg_order_no = row.i8(FieldCode::kFortsRefdataReplMultilegDictLegOrderNo),
                              });
    }

    void apply_matching_row(const RowReader& row) {
        auto& matching = ensure_staged_matching_map();
        const auto key = row.i32(FieldCode::kFortsRefdataReplInstr2matchingMapBaseContractId);
        auto& entry = matching[key];
        entry.base_contract_id = key;
        entry.matching_id = row.i8(FieldCode::kFortsRefdataReplInstr2matchingMapMatchingId);
    }

    void apply_system_message_row(const projection::EventSpec& event, const RowReader& row) {
        auto& messages = ensure_staged_system_messages();
        const auto repl_id = row.i64(FieldCode::kFortsRefdataReplSysMessagesReplId);
        auto& message = messages[repl_id];
        message.repl_id = repl_id;
        message.repl_rev = row.i64(FieldCode::kFortsRefdataReplSysMessagesReplRev, event.signed_value);
        message.repl_act = row.i8(FieldCode::kFortsRefdataReplSysMessagesReplAct);
        message.msg_id = row.i32(FieldCode::kFortsRefdataReplSysMessagesMsgId);
        message.lang_code = row.text(FieldCode::kFortsRefdataReplSysMessagesLangCode);
        message.type_id = row.i32(FieldCode::kFortsRefdataReplSysMessagesTypeId);
        message.moment = row.i64(FieldCode::kFortsRefdataReplSysMessagesMoment);
        message.text = row.text(FieldCode::kFortsRefdataReplSysMessagesText);
        message.urgency = row.i8(FieldCode::kFortsRefdataReplSysMessagesUrgency);
        message.status = row.i8(FieldCode::kFortsRefdataReplSysMessagesStatus);
        message.message_body = row.text(FieldCode::kFortsRefdataReplSysMessagesMessageBody);
        message.source = {.stream_code = StreamCode::kFortsRefdataRepl,
                          .table_code = TableCode::kFortsRefdataReplSysMessages,
                          .repl_rev = event.signed_value,
                          .lifenum = refdata_lifenum().value_or(0),
                          .present = refdata_lifenum().has_value()};
    }

    void apply_trade_heartbeat_row(const RowReader& row) {
        auto& health = ensure_stream_health(ensure_staged_stream_health(), StreamCode::kFortsTradeRepl);
        staged.touched_streams.insert(StreamCode::kFortsTradeRepl);
        health.last_server_time = row.i64(FieldCode::kFortsTradeReplHeartbeatServerTime);
    }

    void apply_sys_event_row(StreamCode stream_code, const RowReader& row, FieldCode event_id_field,
                             FieldCode event_type_field, FieldCode message_field, FieldCode server_time_field) {
        auto& health = ensure_stream_health(ensure_staged_stream_health(), stream_code);
        staged.touched_streams.insert(stream_code);
        health.last_event_id = row.i64(event_id_field);
        health.last_event_type = row.i32(event_type_field);
        health.last_message = row.text(message_field);
        health.last_server_time = row.i64(server_time_field);
    }

    void apply_info_row(StreamCode stream_code, const RowReader& row, std::optional<FieldCode> publication_state_field,
                        FieldCode trades_rev_field, FieldCode trades_lifenum_field,
                        std::optional<FieldCode> server_time_field, std::optional<FieldCode> moment_field) {
        auto& health = ensure_stream_health(ensure_staged_stream_health(), stream_code);
        staged.touched_streams.insert(stream_code);
        if (publication_state_field.has_value()) {
            health.has_publication_state = true;
            health.publication_state = row.i32(*publication_state_field);
        }
        health.last_trades_rev = row.i64(trades_rev_field);
        health.last_trades_lifenum = row.i64(trades_lifenum_field);
        if (server_time_field.has_value()) {
            health.last_server_time = row.i64(*server_time_field);
        }
        if (moment_field.has_value()) {
            health.last_info_moment = row.i64(*moment_field);
        }
        if (stream_code == StreamCode::kFortsUserorderbookRepl && moment_field.has_value()) {
            staged.userbook_regular_info_seen = true;
            staged.userbook_regular_info_publication_state =
                publication_state_field.has_value() ? row.i32(*publication_state_field) : 0;
        }
    }

    void apply_row(const projection::EventSpec& event, const RowReader& row) {
        record_source_revision(event.table_code, row_revision_key(event.table_code, row), event.signed_value);
        switch (event.table_code) {
        case TableCode::kFortsTradeReplOrdersLog:
            apply_trade_order_row(row, false);
            break;
        case TableCode::kFortsTradeReplMultilegOrdersLog:
            apply_trade_order_row(row, true);
            break;
        case TableCode::kFortsTradeReplUserDeal:
            apply_trade_row(row, false);
            break;
        case TableCode::kFortsTradeReplUserMultilegDeal:
            apply_trade_row(row, true);
            break;
        case TableCode::kFortsTradeReplHeartbeat:
            apply_trade_heartbeat_row(row);
            break;
        case TableCode::kFortsTradeReplSysEvents:
            apply_sys_event_row(StreamCode::kFortsTradeRepl, row, FieldCode::kFortsTradeReplSysEventsEventId,
                                FieldCode::kFortsTradeReplSysEventsEventType,
                                FieldCode::kFortsTradeReplSysEventsMessage,
                                FieldCode::kFortsTradeReplSysEventsServerTime);
            break;
        case TableCode::kFortsUserorderbookReplOrders:
            apply_user_book_order_row(row, false, false);
            break;
        case TableCode::kFortsUserorderbookReplMultilegOrders:
            apply_user_book_order_row(row, true, false);
            break;
        case TableCode::kFortsUserorderbookReplOrdersCurrentday:
            apply_user_book_order_row(row, false, true);
            break;
        case TableCode::kFortsUserorderbookReplMultilegOrdersCurrentday:
            apply_user_book_order_row(row, true, true);
            break;
        case TableCode::kFortsUserorderbookReplInfo:
            apply_info_row(
                StreamCode::kFortsUserorderbookRepl, row, FieldCode::kFortsUserorderbookReplInfoPublicationState,
                FieldCode::kFortsUserorderbookReplInfoTradesRev, FieldCode::kFortsUserorderbookReplInfoTradesLifenum,
                std::nullopt, FieldCode::kFortsUserorderbookReplInfoMoment);
            break;
        case TableCode::kFortsUserorderbookReplInfoCurrentday:
            // The current-day table is not the periodic snapshot marker.  Its
            // publication_state must never certify regular USERORDERBOOK.
            staged.touched_streams.insert(StreamCode::kFortsUserorderbookRepl);
            break;
        case TableCode::kFortsPosReplPosition:
            apply_position_row(row);
            break;
        case TableCode::kFortsPosReplInfo:
            apply_info_row(StreamCode::kFortsPosRepl, row, std::nullopt, FieldCode::kFortsPosReplInfoTradesRev,
                           FieldCode::kFortsPosReplInfoTradesLifenum, FieldCode::kFortsPosReplInfoServerTime,
                           std::nullopt);
            break;
        case TableCode::kFortsPartReplPart:
            apply_limit_row(row);
            break;
        case TableCode::kFortsPartReplSysEvents:
            apply_sys_event_row(StreamCode::kFortsPartRepl, row, FieldCode::kFortsPartReplSysEventsEventId,
                                FieldCode::kFortsPartReplSysEventsEventType, FieldCode::kFortsPartReplSysEventsMessage,
                                FieldCode::kFortsPartReplSysEventsServerTime);
            break;
        case TableCode::kFortsRefdataReplSession:
            apply_session_row(row);
            break;
        case TableCode::kFortsRefdataReplFutInstruments:
            apply_future_instrument_row(event, row);
            break;
        case TableCode::kFortsRefdataReplFutVcb:
            apply_future_vcb_row(event, row);
            break;
        case TableCode::kFortsRefdataReplFutSessContents:
            apply_future_session_contents_row(event, row);
            break;
        case TableCode::kFortsSessionstateReplSessionState:
            apply_session_status_row(row);
            break;
        case TableCode::kFortsInstrumentstateReplInstrumentState:
            apply_instrument_status_row(row);
            break;
        case TableCode::kFortsRefdataReplOptSessContents:
            apply_option_instrument_row(row);
            break;
        case TableCode::kFortsRefdataReplMultilegDict:
            apply_multileg_leg_row(row);
            break;
        case TableCode::kFortsRefdataReplInstr2matchingMap:
            apply_matching_row(row);
            break;
        case TableCode::kFortsRefdataReplSysMessages:
            apply_system_message_row(event, row);
            break;
        default:
            break;
        }
    }

    void begin_transaction() {
        staged = {};
        staged.active = true;
    }

    void commit_transaction(const projection::EngineState& state) {
        if (!staged.active) {
            return;
        }

        if (staged.sessions.has_value()) {
            sessions_by_id = std::move(*staged.sessions);
            rebuild_sessions();
        }
        const bool refdata_instruments_changed = staged.instruments.has_value();
        const bool refdata_vcb_changed = staged.future_vcb.has_value();
        if (staged.instruments.has_value())
            instruments_by_isin = std::move(*staged.instruments);
        if (staged.future_sessions.has_value())
            future_sessions = std::move(*staged.future_sessions);
        if (staged.future_vcb.has_value())
            future_vcb_by_row = std::move(*staged.future_vcb);
        if (refdata_instruments_changed || refdata_vcb_changed || staged.sessions.has_value()) {
            select_future_session_membership();
            rebuild_future_vcb();
            rebuild_instruments();
        }
        if (staged.matching_map.has_value()) {
            matching_by_base_contract = std::move(*staged.matching_map);
            rebuild_matching_map();
        }
        if (staged.system_messages.has_value()) {
            system_messages_by_id = std::move(*staged.system_messages);
            rebuild_system_messages();
        }
        if (staged.limits.has_value()) {
            limits_by_key = std::move(*staged.limits);
            rebuild_limits();
        }
        if (staged.positions.has_value()) {
            positions_by_key = std::move(*staged.positions);
            rebuild_positions();
        }
        if (staged.orders.has_value()) {
            orders_by_key = std::move(*staged.orders);
            const auto publish_changes = prepare_row_changes(staged.order_keys.size());
            if (publish_changes && row_changes.orders.empty() && !staged.order_keys.empty())
                row_changes.orders.reserve(
                    std::min(kPrivateRowChangeCapacity, growth_capacity(staged.order_keys.size())));
            for (const auto& key : staged.order_keys) {
                const auto found = orders_by_key.find(key);
                if (found == orders_by_key.end())
                    continue;
                auto& order = found->second;
                // ClearDeleted may remove and recreate the same key inside
                // a transaction. Publish the final row once, then clear its
                // pending markers so a repeated recorded key is skipped.
                if (!order_change_pending(order))
                    continue;
                if (order.trade_repl_commit_sequence == std::numeric_limits<std::uint64_t>::max()) {
                    order.trade_repl_commit_sequence = state.commit_count;
                }
                if (order.user_orderbook_commit_sequence == std::numeric_limits<std::uint64_t>::max()) {
                    order.user_orderbook_commit_sequence = state.commit_count;
                }
                if (publish_changes)
                    row_changes.orders.push_back(order);
                if (native_commit_phase && !staged.rebuild_order_view && !order_snapshots.empty()) {
                    const auto [slot, inserted] = order_view_index.try_emplace(key, order_snapshots.size());
                    if (inserted)
                        order_snapshots.push_back(order);
                    else
                        order_snapshots[slot->second] = order;
                }
            }
            if (!native_commit_phase || staged.rebuild_order_view ||
                (order_snapshots.empty() && !orders_by_key.empty()))
                rebuild_orders();
        }
        if (staged.trades.has_value()) {
            trades_by_key = std::move(*staged.trades);
            std::sort(staged.trade_keys.begin(), staged.trade_keys.end(), [](const TradeKey& lhs, const TradeKey& rhs) {
                return lhs.multileg != rhs.multileg ? lhs.multileg < rhs.multileg : lhs.id_deal < rhs.id_deal;
            });
            staged.trade_keys.erase(std::unique(staged.trade_keys.begin(), staged.trade_keys.end()),
                                    staged.trade_keys.end());
            const auto publish_changes = prepare_row_changes(staged.trade_keys.size());
            if (publish_changes && row_changes.trades.empty() && !staged.trade_keys.empty())
                row_changes.trades.reserve(
                    std::min(kPrivateRowChangeCapacity, growth_capacity(staged.trade_keys.size())));
            for (const auto& key : staged.trade_keys) {
                const auto found = trades_by_key.find(key);
                if (found == trades_by_key.end())
                    continue;
                const auto& trade = found->second;
                if (publish_changes)
                    row_changes.trades.push_back(trade);
                if (native_commit_phase && !staged.rebuild_trade_view && !trade_snapshots.empty()) {
                    const auto [slot, inserted] = trade_view_index.try_emplace(key, trade_snapshots.size());
                    if (inserted)
                        trade_snapshots.push_back(trade);
                    else
                        trade_snapshots[slot->second] = trade;
                }
            }
            if (!native_commit_phase || staged.rebuild_trade_view ||
                (trade_snapshots.empty() && !trades_by_key.empty()))
                rebuild_trades();
        }
        if (staged.rebuild_order_view || staged.rebuild_trade_view)
            prune_row_changes();
        if (staged.stream_health.has_value()) {
            stream_health = std::move(*staged.stream_health);
        }
        if (staged.source_revisions.has_value()) {
            source_revisions = std::move(*staged.source_revisions);
        }
        sync_base_health(state);
        if (staged.userbook_regular_info_seen) {
            auto& health = ensure_stream_health(stream_health, StreamCode::kFortsUserorderbookRepl);
            health.periodic_snapshot_consistent = staged.userbook_regular_info_publication_state == 1;
        }
        for (const auto stream_code : staged.touched_streams) {
            auto& health = ensure_stream_health(stream_health, stream_code);
            health.last_commit_sequence = state.commit_count;
        }
        if (staged.status_bindings_invalidated)
            ++status_binding_generation;
        row_changes.history_lost |= row_changes.resync_required && staged.private_history_retired;
        staged = {};
        native_commit_phase = false;
    }

    void clear_source_revisions_for_stream(StreamCode stream_code) {
        auto& revisions = active_source_revisions();
        const auto belongs_to_stream = [stream_code](TableCode table_code) {
            switch (stream_code) {
            case StreamCode::kFortsTradeRepl:
                return table_code == TableCode::kFortsTradeReplOrdersLog ||
                       table_code == TableCode::kFortsTradeReplMultilegOrdersLog ||
                       table_code == TableCode::kFortsTradeReplUserDeal ||
                       table_code == TableCode::kFortsTradeReplUserMultilegDeal;
            case StreamCode::kFortsUserorderbookRepl:
                return table_code == TableCode::kFortsUserorderbookReplOrders ||
                       table_code == TableCode::kFortsUserorderbookReplMultilegOrders ||
                       table_code == TableCode::kFortsUserorderbookReplOrdersCurrentday ||
                       table_code == TableCode::kFortsUserorderbookReplMultilegOrdersCurrentday;
            case StreamCode::kFortsPosRepl:
                return table_code == TableCode::kFortsPosReplPosition;
            case StreamCode::kFortsPartRepl:
                return table_code == TableCode::kFortsPartReplPart;
            case StreamCode::kFortsRefdataRepl:
                return table_code == TableCode::kFortsRefdataReplSession ||
                       table_code == TableCode::kFortsRefdataReplFutVcb ||
                       table_code == TableCode::kFortsRefdataReplFutInstruments ||
                       table_code == TableCode::kFortsRefdataReplFutSessContents ||
                       table_code == TableCode::kFortsRefdataReplOptSessContents ||
                       table_code == TableCode::kFortsRefdataReplMultilegDict ||
                       table_code == TableCode::kFortsRefdataReplInstr2matchingMap ||
                       table_code == TableCode::kFortsRefdataReplSysMessages;
            case StreamCode::kFortsSessionstateRepl:
                return table_code == TableCode::kFortsSessionstateReplSessionState;
            case StreamCode::kFortsInstrumentstateRepl:
                return table_code == TableCode::kFortsInstrumentstateReplInstrumentState;
            default:
                return false;
            }
        };
        for (auto it = revisions.begin(); it != revisions.end();) {
            if (belongs_to_stream(it->first)) {
                it = revisions.erase(it);
            } else {
                ++it;
            }
        }
    }

    void invalidate_stream_domain(StreamCode stream_code) {
        clear_stream_owned_state(stream_code);
        clear_source_revisions_for_stream(stream_code);
    }

    void invalidate_all_stream_domains() {
        for (const auto& health : stream_health) {
            clear_stream_owned_state(health.stream_code);
            clear_source_revisions_for_stream(health.stream_code);
        }
    }
};

Plaza2PrivateStateProjector::Plaza2PrivateStateProjector() : impl_(std::make_unique<Impl>()) {}

Plaza2PrivateStateProjector::~Plaza2PrivateStateProjector() = default;

Plaza2PrivateStateProjector::Plaza2PrivateStateProjector(Plaza2PrivateStateProjector&&) noexcept = default;

Plaza2PrivateStateProjector& Plaza2PrivateStateProjector::operator=(Plaza2PrivateStateProjector&&) noexcept = default;

Plaza2PrivateStateProjector Plaza2PrivateStateProjector::clone() const {
    Plaza2PrivateStateProjector copy;
    copy.impl_ = std::make_unique<Impl>(*impl_);
    copy.impl_->rebind_limit_index();
    return copy;
}

void Plaza2PrivateStateProjector::reset() {
    impl_->reset();
}

const ConnectorHealthSnapshot& Plaza2PrivateStateProjector::connector_health() const {
    return impl_->connector_health;
}

const ResumeMarkersSnapshot& Plaza2PrivateStateProjector::resume_markers() const {
    return impl_->resume_markers;
}

std::span<const StreamHealthSnapshot> Plaza2PrivateStateProjector::stream_health() const {
    return impl_->stream_health;
}

std::span<const TradingSessionSnapshot> Plaza2PrivateStateProjector::sessions() const {
    return impl_->session_snapshots;
}

std::int32_t Plaza2PrivateStateProjector::current_session_id(std::int64_t now_seconds) const {
    return select_session(sessions(), now_seconds);
}

std::optional<FutureSessionTerms> Plaza2PrivateStateProjector::find_future_session_terms(std::int32_t isin_id) const {
    const auto it = impl_->instruments_by_isin.find(isin_id);
    return it == impl_->instruments_by_isin.end() ? std::nullopt : it->second.future_session_terms;
}

std::optional<FutureSessionTerms>
Plaza2PrivateStateProjector::find_future_session_terms(std::int32_t isin_id, std::int32_t sess_id,
                                                       std::uint64_t expected_lifenum) const {
    const auto it = impl_->future_sessions.find(future_session_key(isin_id, sess_id));
    if (it == impl_->future_sessions.end() || !it->second.future_session_terms)
        return std::nullopt;
    const auto& terms = *it->second.future_session_terms;
    if (!terms.source.present || terms.source.lifenum != expected_lifenum ||
        impl_->refdata_lifenum() != expected_lifenum)
        return std::nullopt;
    return terms;
}

std::span<const InstrumentSnapshot> Plaza2PrivateStateProjector::instruments() const {
    return impl_->instrument_snapshots;
}

std::span<const FutureVcbSnapshot> Plaza2PrivateStateProjector::future_vcb() const {
    return impl_->future_vcb_snapshots;
}

std::span<const MatchingMapSnapshot> Plaza2PrivateStateProjector::matching_map() const {
    return impl_->matching_snapshots;
}

std::span<const SystemMessageSnapshot> Plaza2PrivateStateProjector::system_messages() const {
    return impl_->system_message_snapshots;
}

LimitParticipantKind classify_limit_participant(std::string_view code) noexcept {
    // Protocol length describes participant level, not account authorization.
    if (code.size() == 4)
        return LimitParticipantKind::BrokerageFirm;
    if (code.size() == 7)
        return LimitParticipantKind::Client;
    return LimitParticipantKind::Unknown;
}
LimitLookup Plaza2PrivateStateProjector::find_limit_by_code(const std::string& code) const {
    const auto it = impl_->limit_index.find(code);
    return it == impl_->limit_index.end() ? LimitLookup{} : it->second;
}
std::size_t Plaza2PrivateStateProjector::limit_row_count() const noexcept {
    return impl_->limits_by_key.size();
}
std::size_t Plaza2PrivateStateProjector::unknown_limit_row_count() const noexcept {
    return impl_->unknown_limits;
}

std::span<const LimitSnapshot> Plaza2PrivateStateProjector::limits() const {
    return impl_->limit_snapshots;
}

std::span<const PositionSnapshot> Plaza2PrivateStateProjector::positions() const {
    return impl_->position_snapshots;
}

std::span<const OwnOrderSnapshot> Plaza2PrivateStateProjector::own_orders() const {
    return impl_->order_snapshots;
}

std::span<const OwnTradeSnapshot> Plaza2PrivateStateProjector::own_trades() const {
    return impl_->trade_snapshots;
}

PrivateRowChanges Plaza2PrivateStateProjector::take_row_changes() {
    auto changes = std::move(impl_->row_changes);
    impl_->row_changes = {};
    return changes;
}

PrivateStorageCapacity Plaza2PrivateStateProjector::storage_capacity() const noexcept {
    PrivateStorageCapacity capacity{.order_buckets = impl_->orders_by_key.bucket_count(),
                                    .order_identity_buckets = impl_->orders_by_key.identities.bucket_count(),
                                    .order_view_buckets = impl_->order_view_index.bucket_count(),
                                    .trade_buckets = impl_->trades_by_key.bucket_count(),
                                    .trade_view_buckets = impl_->trade_view_index.bucket_count(),
                                    .source_table_buckets = impl_->source_revisions.bucket_count(),
                                    .order_snapshot_capacity = impl_->order_snapshots.capacity(),
                                    .trade_snapshot_capacity = impl_->trade_snapshots.capacity()};
    for (const auto& [table, rows] : impl_->source_revisions)
        capacity.source_row_buckets += rows.bucket_count();
    return capacity;
}

std::optional<SourceRowProvenance>
Plaza2PrivateStateProjector::instrument_source_provenance(generated::TableCode table_code, std::int32_t isin_id) const {
    if (table_code != generated::TableCode::kFortsRefdataReplFutInstruments &&
        table_code != generated::TableCode::kFortsRefdataReplFutSessContents) {
        return std::nullopt;
    }
    return impl_->refdata_source_provenance(table_code, isin_id);
}

std::optional<SourceRowProvenance>
Plaza2PrivateStateProjector::session_source_provenance(generated::TableCode table_code, std::int32_t sess_id) const {
    if (table_code != generated::TableCode::kFortsRefdataReplSession) {
        return std::nullopt;
    }
    return impl_->refdata_source_provenance(table_code, sess_id);
}

std::optional<std::uint64_t> Plaza2PrivateStateProjector::refdata_lifenum() const {
    return impl_->refdata_lifenum();
}

std::uint64_t Plaza2PrivateStateProjector::status_binding_generation() const {
    return impl_->status_binding_generation;
}

void Plaza2PrivateStateProjector::reset_stream_snapshot(generated::StreamCode stream_code) {
    impl_->invalidate_stream_domain(stream_code);
    impl_->invalidate_closed_stream(stream_code);
}

void Plaza2PrivateStateProjector::reset_status_snapshot(generated::StreamCode stream_code) {
    if (stream_code != StreamCode::kFortsSessionstateRepl && stream_code != StreamCode::kFortsInstrumentstateRepl)
        return;
    impl_->invalidate_stream_domain(stream_code);
    impl_->invalidate_closed_stream(stream_code);
}

void Plaza2PrivateStateProjector::invalidate_periodic_snapshot(generated::StreamCode stream_code,
                                                               generated::TableCode table_code) {
    impl_->invalidate_periodic_snapshot(stream_code, table_code);
}

void Plaza2PrivateStateProjector::on_event(const projection::ScenarioSpec&, const projection::EventSpec& event,
                                           const projection::EngineState& state) {
    switch (event.kind) {
    case projection::EventKind::kOpen:
        impl_->sync_base_health(state);
        break;
    case projection::EventKind::kClose:
        impl_->sync_base_health(state);
        impl_->invalidate_closed_stream(event.stream_code);
        break;
    case projection::EventKind::kSnapshotBegin:
        impl_->sync_base_health(state);
        break;
    case projection::EventKind::kSnapshotEnd:
        impl_->sync_base_health(state);
        break;
    case projection::EventKind::kOnline:
        impl_->sync_base_health(state);
        break;
    case projection::EventKind::kTransactionBegin:
        impl_->native_commit_phase = event.numeric_value == 1;
        impl_->begin_transaction();
        impl_->sync_base_health(state);
        break;
    case projection::EventKind::kTransactionCommit:
        break;
    case projection::EventKind::kStreamData:
        break;
    case projection::EventKind::kReplState:
        impl_->sync_base_health(state);
        break;
    case projection::EventKind::kLifeNum:
        impl_->sync_base_health(state);
        if (event.stream_code == projection::kNoStreamCode) {
            impl_->invalidate_all_stream_domains();
        } else {
            const auto known = impl_->lifenums_by_stream.find(event.stream_code);
            if (known == impl_->lifenums_by_stream.end()) {
                impl_->lifenums_by_stream.emplace(event.stream_code, event.numeric_value);
            } else if (known->second != event.numeric_value) {
                known->second = event.numeric_value;
                impl_->invalidate_stream_domain(event.stream_code);
            }
        }
        break;
    case projection::EventKind::kClearDeleted:
        impl_->sync_base_health(state);
        if (event.table_code != projection::kNoTableCode) {
            impl_->clear_table_owned_state(event.table_code, event.signed_value);
        } else if (impl_->staged.active) {
            impl_->stage_clear_stream_owned_state(event.stream_code);
        } else {
            impl_->clear_stream_owned_state(event.stream_code);
        }
        break;
    }
}

void Plaza2PrivateStateProjector::on_stream_row(const projection::ScenarioSpec&, const projection::EventSpec& event,
                                                const projection::RowSpec&,
                                                std::span<const projection::FieldValueSpec> fields,
                                                const projection::EngineState&) {
    if (!impl_->staged.active) {
        return;
    }
    impl_->apply_row(event, RowReader{fields});
}

void Plaza2PrivateStateProjector::on_transaction_commit(const projection::ScenarioSpec&, const projection::EventSpec&,
                                                        const projection::EngineState& state) {
    impl_->commit_transaction(state);
}

} // namespace moex::plaza2::private_state
