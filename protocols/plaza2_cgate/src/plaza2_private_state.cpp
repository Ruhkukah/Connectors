#include "moex/plaza2/cgate/plaza2_private_state.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace moex::plaza2::private_state {

namespace {

using StreamCode = generated::StreamCode;
using TableCode = generated::TableCode;
using FieldCode = generated::FieldCode;

struct EnumClassHash {
    template <typename T> std::size_t operator()(T value) const {
        return std::hash<std::uint32_t>{}(static_cast<std::uint32_t>(value));
    }
};

template <typename T> void hash_combine(std::size_t& seed, const T& value) {
    seed ^= std::hash<T>{}(value) + 0x9E3779B97F4A7C15ULL + (seed << 6U) + (seed >> 2U);
}

struct LimitKey {
    PositionScope scope{PositionScope::kClient};
    std::string account_code;

    bool operator==(const LimitKey& other) const {
        return scope == other.scope && account_code == other.account_code;
    }
};

struct LimitKeyHash {
    std::size_t operator()(const LimitKey& key) const {
        std::size_t seed = 0;
        hash_combine(seed, static_cast<std::uint32_t>(key.scope));
        hash_combine(seed, key.account_code);
        return seed;
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
    bool multileg{false};
    std::int64_t public_order_id{0};
    std::int64_t private_order_id{0};
    std::int32_t ext_id{0};
    std::string client_code;

    bool operator==(const OrderKey& other) const {
        return multileg == other.multileg && public_order_id == other.public_order_id &&
               private_order_id == other.private_order_id && ext_id == other.ext_id && client_code == other.client_code;
    }
};

struct OrderKeyHash {
    std::size_t operator()(const OrderKey& key) const {
        std::size_t seed = 0;
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
using MatchingMap = std::unordered_map<std::int32_t, MatchingMapSnapshot>;
using LimitMap = std::unordered_map<LimitKey, LimitSnapshot, LimitKeyHash>;
using PositionMap = std::unordered_map<PositionKey, PositionSnapshot, PositionKeyHash>;
using OrderMap = std::unordered_map<OrderKey, OwnOrderSnapshot, OrderKeyHash>;
using TradeMap = std::unordered_map<TradeKey, OwnTradeSnapshot, TradeKeyHash>;

struct RowReader {
    std::span<const fake::FieldValueSpec> fields;

    const fake::FieldValueSpec* find(FieldCode code) const {
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
        if (field->kind == fake::ValueKind::kUnsignedInteger || field->kind == fake::ValueKind::kTimestamp) {
            return static_cast<std::int64_t>(field->unsigned_value);
        }
        return field->signed_value;
    }

    std::uint64_t u64(FieldCode code, std::uint64_t fallback = 0) const {
        const auto* field = find(code);
        if (field == nullptr) {
            return fallback;
        }
        if (field->kind == fake::ValueKind::kSignedInteger) {
            return field->signed_value < 0 ? fallback : static_cast<std::uint64_t>(field->signed_value);
        }
        return field->unsigned_value;
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

template <typename Map> Map& ensure_stage_copy(std::optional<Map>& staged, const Map& committed) {
    if (!staged.has_value()) {
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
std::vector<Snapshot> sorted_values(const Map& map, Comparator comparator) {
    std::vector<Snapshot> out;
    out.reserve(map.size());
    for (const auto& [_, value] : map) {
        out.push_back(value);
    }
    std::sort(out.begin(), out.end(), comparator);
    return out;
}

bool order_has_any_source(const OwnOrderSnapshot& order) {
    return order.from_trade_repl || order.from_user_book || order.from_current_day;
}

void clear_trade_source(OrderMap& orders) {
    for (auto it = orders.begin(); it != orders.end();) {
        it->second.from_trade_repl = false;
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
    bool active{false};
    std::optional<SessionMap> sessions;
    std::optional<InstrumentMap> instruments;
    std::optional<MatchingMap> matching_map;
    std::optional<LimitMap> limits;
    std::optional<PositionMap> positions;
    std::optional<OrderMap> orders;
    std::optional<TradeMap> trades;
    std::optional<std::vector<StreamHealthSnapshot>> stream_health;
    std::unordered_set<StreamCode, EnumClassHash> touched_streams;
};

} // namespace

struct Plaza2PrivateStateProjector::Impl {
    ConnectorHealthSnapshot connector_health;
    ResumeMarkersSnapshot resume_markers;
    std::vector<StreamHealthSnapshot> stream_health;

    SessionMap sessions_by_id;
    InstrumentMap instruments_by_isin;
    MatchingMap matching_by_base_contract;
    LimitMap limits_by_key;
    PositionMap positions_by_key;
    OrderMap orders_by_key;
    TradeMap trades_by_key;

    std::vector<TradingSessionSnapshot> session_snapshots;
    std::vector<InstrumentSnapshot> instrument_snapshots;
    std::vector<MatchingMapSnapshot> matching_snapshots;
    std::vector<LimitSnapshot> limit_snapshots;
    std::vector<PositionSnapshot> position_snapshots;
    std::vector<OwnOrderSnapshot> order_snapshots;
    std::vector<OwnTradeSnapshot> trade_snapshots;

    StagedState staged;

    void reset() {
        connector_health = {};
        resume_markers = {};
        stream_health.clear();
        sessions_by_id.clear();
        instruments_by_isin.clear();
        matching_by_base_contract.clear();
        limits_by_key.clear();
        positions_by_key.clear();
        orders_by_key.clear();
        trades_by_key.clear();
        session_snapshots.clear();
        instrument_snapshots.clear();
        matching_snapshots.clear();
        limit_snapshots.clear();
        position_snapshots.clear();
        order_snapshots.clear();
        trade_snapshots.clear();
        staged = {};
    }

    void sync_base_health(const fake::EngineState& state) {
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

    std::vector<StreamHealthSnapshot>& ensure_staged_stream_health() {
        if (!staged.stream_health.has_value()) {
            staged.stream_health = stream_health;
        }
        return *staged.stream_health;
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

    void rebuild_instruments() {
        instrument_snapshots = sorted_values<InstrumentSnapshot>(
            instruments_by_isin,
            [](const InstrumentSnapshot& lhs, const InstrumentSnapshot& rhs) { return lhs.isin_id < rhs.isin_id; });
    }

    void rebuild_matching_map() {
        matching_snapshots = sorted_values<MatchingMapSnapshot>(
            matching_by_base_contract, [](const MatchingMapSnapshot& lhs, const MatchingMapSnapshot& rhs) {
                return lhs.base_contract_id < rhs.base_contract_id;
            });
    }

    void rebuild_limits() {
        limit_snapshots =
            sorted_values<LimitSnapshot>(limits_by_key, [](const LimitSnapshot& lhs, const LimitSnapshot& rhs) {
                if (lhs.scope != rhs.scope) {
                    return static_cast<std::uint32_t>(lhs.scope) < static_cast<std::uint32_t>(rhs.scope);
                }
                return lhs.account_code < rhs.account_code;
            });
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

    void rebuild_orders() {
        order_snapshots = sorted_values<OwnOrderSnapshot>(orders_by_key,
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
                                                              return lhs.ext_id < rhs.ext_id;
                                                          });
    }

    void rebuild_trades() {
        trade_snapshots = sorted_values<OwnTradeSnapshot>(trades_by_key,
                                                          [](const OwnTradeSnapshot& lhs, const OwnTradeSnapshot& rhs) {
                                                              if (lhs.multileg != rhs.multileg) {
                                                                  return lhs.multileg < rhs.multileg;
                                                              }
                                                              return lhs.id_deal < rhs.id_deal;
                                                          });
    }

    void rebuild_all_snapshots() {
        rebuild_sessions();
        rebuild_instruments();
        rebuild_matching_map();
        rebuild_limits();
        rebuild_positions();
        rebuild_orders();
        rebuild_trades();
    }

    void clear_stream_owned_state(StreamCode stream_code) {
        switch (stream_code) {
        case StreamCode::kFortsTradeRepl:
            clear_trade_source(orders_by_key);
            trades_by_key.clear();
            rebuild_orders();
            rebuild_trades();
            break;
        case StreamCode::kFortsUserorderbookRepl:
            clear_user_book_source(orders_by_key);
            rebuild_orders();
            break;
        case StreamCode::kFortsPosRepl:
            positions_by_key.clear();
            rebuild_positions();
            break;
        case StreamCode::kFortsPartRepl:
            limits_by_key.clear();
            rebuild_limits();
            break;
        case StreamCode::kFortsRefdataRepl:
            sessions_by_id.clear();
            instruments_by_isin.clear();
            matching_by_base_contract.clear();
            rebuild_sessions();
            rebuild_instruments();
            rebuild_matching_map();
            break;
        default:
            break;
        }

        auto& health = ensure_stream_health(stream_health, stream_code);
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
    }

    OrderMap& ensure_staged_orders() {
        staged.touched_streams.insert(StreamCode::kFortsTradeRepl);
        staged.touched_streams.insert(StreamCode::kFortsUserorderbookRepl);
        return ensure_stage_copy(staged.orders, orders_by_key);
    }

    SessionMap& ensure_staged_sessions() {
        staged.touched_streams.insert(StreamCode::kFortsRefdataRepl);
        return ensure_stage_copy(staged.sessions, sessions_by_id);
    }

    InstrumentMap& ensure_staged_instruments() {
        staged.touched_streams.insert(StreamCode::kFortsRefdataRepl);
        return ensure_stage_copy(staged.instruments, instruments_by_isin);
    }

    MatchingMap& ensure_staged_matching_map() {
        staged.touched_streams.insert(StreamCode::kFortsRefdataRepl);
        return ensure_stage_copy(staged.matching_map, matching_by_base_contract);
    }

    LimitMap& ensure_staged_limits() {
        staged.touched_streams.insert(StreamCode::kFortsPartRepl);
        return ensure_stage_copy(staged.limits, limits_by_key);
    }

    PositionMap& ensure_staged_positions() {
        staged.touched_streams.insert(StreamCode::kFortsPosRepl);
        return ensure_stage_copy(staged.positions, positions_by_key);
    }

    TradeMap& ensure_staged_trades() {
        staged.touched_streams.insert(StreamCode::kFortsTradeRepl);
        return ensure_stage_copy(staged.trades, trades_by_key);
    }

    void apply_trade_order_row(const RowReader& row, bool multileg) {
        auto& orders = ensure_staged_orders();
        OrderKey key{
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
        auto& order = orders[key];
        order.multileg = multileg;
        order.public_order_id = key.public_order_id;
        order.private_order_id = key.private_order_id;
        order.ext_id = key.ext_id;
        order.client_code = key.client_code;
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
        order.from_trade_repl = true;
    }

    void apply_user_book_order_row(const RowReader& row, bool multileg, bool current_day) {
        auto& orders = ensure_staged_orders();
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
            .multileg = multileg,
            .public_order_id = row.i64(public_order_id_field),
            .private_order_id = row.i64(private_order_id_field),
            .ext_id = row.i32(ext_id_field),
            .client_code = row.text(client_code_field),
        };
        auto& order = orders[key];
        order.multileg = multileg;
        order.public_order_id = key.public_order_id;
        order.private_order_id = key.private_order_id;
        order.ext_id = key.ext_id;
        order.client_code = key.client_code;

        order.sess_id = row.i32(sess_field);
        order.isin_id = row.i32(isin_field);
        order.login_from = row.text(login_field);
        order.comment = row.text(comment_field);
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
        order.from_user_book = !current_day;
        order.from_current_day = current_day;
    }

    void apply_trade_row(const RowReader& row, bool multileg) {
        auto& trades = ensure_staged_trades();
        const auto key = TradeKey{
            .multileg = multileg,
            .id_deal = row.i64(multileg ? FieldCode::kFortsTradeReplUserMultilegDealIdDeal
                                        : FieldCode::kFortsTradeReplUserDealIdDeal),
        };
        auto& trade = trades[key];
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
            .scope = PositionScope::kClient,
            .account_code = row.text(FieldCode::kFortsPartReplPartClientCode),
        };
        auto& limit = limits[key];
        limit.scope = PositionScope::kClient;
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

    void apply_future_instrument_row(const RowReader& row) {
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
        instrument.settlement_price = row.text(FieldCode::kFortsRefdataReplFutInstrumentsSettlementPrice);
        instrument.last_trade_date = row.i64(FieldCode::kFortsRefdataReplFutInstrumentsLastTradeDate);
        instrument.group_mask = row.i64(FieldCode::kFortsRefdataReplFutInstrumentsGroupMask);
        instrument.trade_period_access = row.i64(FieldCode::kFortsRefdataReplFutInstrumentsTradePeriodAccess);
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
    }

    void apply_row(const fake::EventSpec& event, const RowReader& row) {
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
            apply_info_row(StreamCode::kFortsUserorderbookRepl, row,
                           FieldCode::kFortsUserorderbookReplInfoCurrentdayPublicationState,
                           FieldCode::kFortsUserorderbookReplInfoCurrentdayTradesRev,
                           FieldCode::kFortsUserorderbookReplInfoCurrentdayTradesLifenum,
                           FieldCode::kFortsUserorderbookReplInfoCurrentdayServerTime, std::nullopt);
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
            apply_future_instrument_row(row);
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
        default:
            break;
        }
    }

    void begin_transaction() {
        staged = {};
        staged.active = true;
    }

    void commit_transaction(const fake::EngineState& state) {
        if (!staged.active) {
            return;
        }

        if (staged.sessions.has_value()) {
            sessions_by_id = std::move(*staged.sessions);
            rebuild_sessions();
        }
        if (staged.instruments.has_value()) {
            instruments_by_isin = std::move(*staged.instruments);
            rebuild_instruments();
        }
        if (staged.matching_map.has_value()) {
            matching_by_base_contract = std::move(*staged.matching_map);
            rebuild_matching_map();
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
            rebuild_orders();
        }
        if (staged.trades.has_value()) {
            trades_by_key = std::move(*staged.trades);
            rebuild_trades();
        }
        if (staged.stream_health.has_value()) {
            stream_health = std::move(*staged.stream_health);
        }
        sync_base_health(state);
        for (const auto stream_code : staged.touched_streams) {
            auto& health = ensure_stream_health(stream_health, stream_code);
            health.last_commit_sequence = state.commit_count;
        }
        staged = {};
    }

    void invalidate_all_stream_domains() {
        for (const auto& health : stream_health) {
            clear_stream_owned_state(health.stream_code);
        }
    }
};

Plaza2PrivateStateProjector::Plaza2PrivateStateProjector() : impl_(std::make_unique<Impl>()) {}

Plaza2PrivateStateProjector::~Plaza2PrivateStateProjector() = default;

Plaza2PrivateStateProjector::Plaza2PrivateStateProjector(Plaza2PrivateStateProjector&&) noexcept = default;

Plaza2PrivateStateProjector& Plaza2PrivateStateProjector::operator=(Plaza2PrivateStateProjector&&) noexcept = default;

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

std::span<const InstrumentSnapshot> Plaza2PrivateStateProjector::instruments() const {
    return impl_->instrument_snapshots;
}

std::span<const MatchingMapSnapshot> Plaza2PrivateStateProjector::matching_map() const {
    return impl_->matching_snapshots;
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

void Plaza2PrivateStateProjector::on_event(const fake::ScenarioSpec&, const fake::EventSpec& event,
                                           const fake::EngineState& state) {
    switch (event.kind) {
    case fake::EventKind::kOpen:
        impl_->sync_base_health(state);
        break;
    case fake::EventKind::kClose:
        impl_->sync_base_health(state);
        break;
    case fake::EventKind::kSnapshotBegin:
        impl_->sync_base_health(state);
        break;
    case fake::EventKind::kSnapshotEnd:
        impl_->sync_base_health(state);
        break;
    case fake::EventKind::kOnline:
        impl_->sync_base_health(state);
        break;
    case fake::EventKind::kTransactionBegin:
        impl_->begin_transaction();
        impl_->sync_base_health(state);
        break;
    case fake::EventKind::kTransactionCommit:
        break;
    case fake::EventKind::kStreamData:
        break;
    case fake::EventKind::kReplState:
        impl_->sync_base_health(state);
        break;
    case fake::EventKind::kLifeNum:
        impl_->sync_base_health(state);
        impl_->invalidate_all_stream_domains();
        break;
    case fake::EventKind::kClearDeleted:
        impl_->sync_base_health(state);
        impl_->clear_stream_owned_state(event.stream_code);
        break;
    }
}

void Plaza2PrivateStateProjector::on_stream_row(const fake::ScenarioSpec&, const fake::EventSpec& event,
                                                const fake::RowSpec&, std::span<const fake::FieldValueSpec> fields,
                                                const fake::EngineState&) {
    if (!impl_->staged.active) {
        return;
    }
    impl_->apply_row(event, RowReader{fields});
}

void Plaza2PrivateStateProjector::on_transaction_commit(const fake::ScenarioSpec&, const fake::EventSpec&,
                                                        const fake::EngineState& state) {
    impl_->commit_transaction(state);
}

} // namespace moex::plaza2::private_state
