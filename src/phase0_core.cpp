#include "moex_c_api_internal.hpp"
#include "moex_core/logging/redaction.hpp"
#include "moex_core/phase0_core.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moex::phase0 {

namespace {

std::string to_lower(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lowered;
}

} // namespace

BuildInfo build_info() {
    return BuildInfo{
        .project_version = MOEX_PHASE0_PROJECT_VERSION,
        .source_root = MOEX_PHASE0_SOURCE_ROOT,
    };
}

bool prod_requires_arm(std::string_view environment, bool armed) {
    return to_lower(environment) != "prod" || armed;
}

BackpressureCounters make_seed_counters(EventLossPolicy policy) {
    BackpressureCounters counters{};
    counters.high_watermark = policy == EventLossPolicy::Lossless ? 1 : 0;
    counters.overflowed = false;
    return counters;
}

} // namespace moex::phase0

namespace moex::logging {

std::string redact_value(SecretKind kind, const std::string& value) {
    if (value.empty()) {
        return {};
    }

    if (kind == SecretKind::AccountIdentifier && value.size() > 4) {
        return value.substr(0, 2) + "***" + value.substr(value.size() - 2);
    }

    return "[REDACTED]";
}

} // namespace moex::logging

namespace {

constexpr std::int64_t kMonotonicBaseNs = 1'000'000'000LL;
constexpr std::int64_t kStepNs = 1'000'000LL;

constexpr std::uint8_t to_abi_bool(bool value) {
    return value ? 1U : 0U;
}

std::string trim_copy(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::vector<std::string> split_line(std::string_view line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= line.size()) {
        const auto next = line.find('|', start);
        if (next == std::string_view::npos) {
            parts.emplace_back(trim_copy(line.substr(start)));
            break;
        }
        parts.emplace_back(trim_copy(line.substr(start, next - start)));
        start = next + 1;
    }
    return parts;
}

void copy_fixed(std::string_view value, char* dest, std::size_t capacity) {
    std::memset(dest, 0, capacity);
    if (capacity == 0) {
        return;
    }
    const auto count = std::min(value.size(), capacity - 1);
    if (count > 0) {
        std::memcpy(dest, value.data(), count);
    }
}

std::int64_t parse_int64(std::string_view value, std::int64_t fallback = 0) {
    try {
        return value.empty() ? fallback : std::stoll(std::string(value));
    } catch (...) {
        return fallback;
    }
}

double parse_double(std::string_view value, double fallback = 0.0) {
    try {
        return value.empty() ? fallback : std::stod(std::string(value));
    } catch (...) {
        return fallback;
    }
}

bool parse_bool_flag(std::string_view value) {
    const auto lowered = moex::phase0::to_lower(value);
    return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "y";
}

std::int32_t parse_side(std::string_view value) {
    const auto lowered = moex::phase0::to_lower(value);
    if (lowered == "bid" || lowered == "buy") {
        return 1;
    }
    if (lowered == "ask" || lowered == "sell") {
        return 2;
    }
    return 0;
}

std::int32_t parse_status(std::string_view value) {
    const auto lowered = moex::phase0::to_lower(value);
    if (lowered == "new") {
        return MOEX_NATIVE_ORDER_STATUS_NEW;
    }
    if (lowered == "partial_fill" || lowered == "partialfill") {
        return MOEX_NATIVE_ORDER_STATUS_PARTIAL_FILL;
    }
    if (lowered == "filled") {
        return MOEX_NATIVE_ORDER_STATUS_FILLED;
    }
    if (lowered == "canceled" || lowered == "cancelled") {
        return MOEX_NATIVE_ORDER_STATUS_CANCELED;
    }
    if (lowered == "rejected") {
        return MOEX_NATIVE_ORDER_STATUS_REJECTED;
    }
    return MOEX_NATIVE_ORDER_STATUS_UNSPECIFIED;
}

std::int64_t ms_to_ns(std::int64_t value) {
    return value <= 0 ? 0 : value * 1'000'000LL;
}

MoexPolledEvent make_event(MoexEventType event_type, std::uint64_t sequence, std::int64_t event_time_ms,
                           bool read_only_shadow) {
    MoexPolledEvent event{};
    event.header.struct_size = static_cast<std::uint32_t>(sizeof(MoexEventHeader));
    event.header.abi_version = MOEX_C_ABI_VERSION;
    event.header.event_type = static_cast<std::uint16_t>(event_type);
    event.header.connector_seq = sequence;
    event.header.source_seq = sequence;
    const auto monotonic = kMonotonicBaseNs + static_cast<std::int64_t>(sequence) * kStepNs;
    event.header.monotonic_time_ns = monotonic;
    event.header.exchange_time_utc_ns = ms_to_ns(event_time_ms);
    event.header.source_time_utc_ns = ms_to_ns(event_time_ms);
    event.header.socket_receive_monotonic_ns = monotonic;
    event.header.decode_monotonic_ns = monotonic + 1'000LL;
    event.header.publish_monotonic_ns = monotonic + 2'000LL;
    event.header.managed_poll_monotonic_ns = 0;
    event.header.source_connector = MOEX_SOURCE_UNKNOWN;
    event.payload_size = static_cast<std::uint32_t>(sizeof(MoexPolledEvent));
    event.payload_version = 1;
    event.read_only_shadow = read_only_shadow ? 1U : 0U;
    return event;
}

MoexPolledEvent make_status_event(std::uint64_t sequence, std::string_view info_text, MoexReplayState replay_state) {
    auto event = make_event(replay_state == MOEX_REPLAY_STATE_UNSPECIFIED ? MOEX_EVENT_CONNECTOR_STATUS
                                                                          : MOEX_EVENT_REPLAY_STATE,
                            sequence, 0, true);
    event.replay_state = static_cast<std::uint16_t>(replay_state);
    copy_fixed(info_text, event.info_text, MOEX_INFO_CAPACITY);
    return event;
}

bool parse_replay_file(const std::string& path, std::vector<MoexPolledEvent>& events, std::string& error) {
    std::ifstream input(path);
    if (!input.is_open()) {
        error = "unable to open replay fixture";
        return false;
    }

    events.clear();
    std::string line;
    std::uint64_t sequence = 1;
    while (std::getline(input, line)) {
        const auto trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        const auto parts = split_line(trimmed);
        if (parts.empty()) {
            continue;
        }

        const auto kind = moex::phase0::to_lower(parts[0]);
        if (kind == "instrument" && parts.size() >= 6) {
            auto event = make_event(MOEX_EVENT_INSTRUMENT, sequence++, parse_int64(parts[5]), true);
            copy_fixed(parts[1], event.symbol, MOEX_SYMBOL_CAPACITY);
            copy_fixed(parts[2], event.board, MOEX_BOARD_CAPACITY);
            copy_fixed(parts[3], event.instrument_group, MOEX_GROUP_CAPACITY);
            event.prefer_order_book_l1 = parse_bool_flag(parts[4]) ? 1U : 0U;
            events.push_back(event);
            continue;
        }

        if (kind == "l1" && parts.size() >= 8) {
            auto event = make_event(MOEX_EVENT_PUBLIC_L1, sequence++, parse_int64(parts[7]), true);
            copy_fixed(parts[1], event.symbol, MOEX_SYMBOL_CAPACITY);
            copy_fixed(parts[2], event.board, MOEX_BOARD_CAPACITY);
            event.price = parse_double(parts[3]);
            event.quantity = parse_double(parts[4]);
            event.secondary_price = parse_double(parts[5]);
            event.secondary_quantity = parse_double(parts[6]);
            events.push_back(event);
            continue;
        }

        if (kind == "public_trade" && parts.size() >= 10) {
            auto event = make_event(MOEX_EVENT_PUBLIC_TRADE, sequence++, parse_int64(parts[8]), true);
            copy_fixed(parts[1], event.symbol, MOEX_SYMBOL_CAPACITY);
            copy_fixed(parts[2], event.board, MOEX_BOARD_CAPACITY);
            copy_fixed(parts[3], event.instrument_group, MOEX_GROUP_CAPACITY);
            event.side = parse_side(parts[4]);
            event.quantity = parse_double(parts[5]);
            event.price = parse_double(parts[6]);
            copy_fixed(parts[7], event.info_text, MOEX_INFO_CAPACITY);
            event.existing = parts.size() >= 10 && parse_bool_flag(parts[9]) ? 1U : 0U;
            events.push_back(event);
            continue;
        }

        if (kind == "order_book" && parts.size() >= 9) {
            auto event = make_event(MOEX_EVENT_ORDER_BOOK, sequence++, parse_int64(parts[8]), true);
            copy_fixed(parts[1], event.symbol, MOEX_SYMBOL_CAPACITY);
            copy_fixed(parts[2], event.board, MOEX_BOARD_CAPACITY);
            event.side = parse_side(parts[3]);
            event.level = static_cast<std::int32_t>(parse_int64(parts[4]));
            event.price = parse_double(parts[5]);
            event.quantity = parse_double(parts[6]);
            event.update_type = static_cast<std::int32_t>(parse_int64(parts[7]));
            events.push_back(event);
            continue;
        }

        if (kind == "order_status" && parts.size() >= 10) {
            auto event = make_event(MOEX_EVENT_ORDER_STATUS, sequence++, parse_int64(parts[9]), true);
            copy_fixed(parts[1], event.server_order_id, MOEX_ORDER_ID_CAPACITY);
            copy_fixed(parts[2], event.client_order_id, MOEX_ORDER_ID_CAPACITY);
            copy_fixed(parts[3], event.symbol, MOEX_SYMBOL_CAPACITY);
            event.status = parse_status(parts[4]);
            event.cumulative_quantity = parse_double(parts[5]);
            event.remaining_quantity = parse_double(parts[6]);
            event.price = parse_double(parts[7]);
            copy_fixed(parts[8], event.info_text, MOEX_INFO_CAPACITY);
            events.push_back(event);
            continue;
        }

        if (kind == "private_trade" && parts.size() >= 11) {
            auto event = make_event(MOEX_EVENT_PRIVATE_TRADE, sequence++, parse_int64(parts[10]), true);
            copy_fixed(parts[1], event.server_order_id, MOEX_ORDER_ID_CAPACITY);
            copy_fixed(parts[2], event.client_order_id, MOEX_ORDER_ID_CAPACITY);
            copy_fixed(parts[3], event.symbol, MOEX_SYMBOL_CAPACITY);
            copy_fixed(parts[4], event.trade_account, MOEX_ACCOUNT_CAPACITY);
            event.quantity = parse_double(parts[5]);
            event.cumulative_quantity = parse_double(parts[6]);
            event.price = parse_double(parts[7]);
            event.average_price = parse_double(parts[8]);
            copy_fixed(parts[9], event.info_text, MOEX_INFO_CAPACITY);
            events.push_back(event);
            continue;
        }

        if (kind == "position" && parts.size() >= 8) {
            auto event = make_event(MOEX_EVENT_POSITION, sequence++, parse_int64(parts[7]), true);
            copy_fixed(parts[1], event.portfolio, MOEX_PORTFOLIO_CAPACITY);
            copy_fixed(parts[2], event.symbol, MOEX_SYMBOL_CAPACITY);
            copy_fixed(parts[3], event.exchange, MOEX_EXCHANGE_CAPACITY);
            event.quantity = parse_double(parts[4]);
            event.average_price = parse_double(parts[5]);
            event.open_profit_loss = parse_double(parts[6]);
            events.push_back(event);
            continue;
        }

        error = "unsupported replay fixture line: " + trimmed;
        return false;
    }

    events.push_back(make_status_event(sequence, "synthetic replay drained", MOEX_REPLAY_STATE_DRAINED));
    return true;
}

} // namespace

namespace {

using moex::plaza2::private_state::ConnectorHealthSnapshot;
using moex::plaza2::private_state::InstrumentKind;
using moex::plaza2::private_state::InstrumentSnapshot;
using moex::plaza2::private_state::LimitSnapshot;
using moex::plaza2::private_state::MatchingMapSnapshot;
using moex::plaza2::private_state::OwnOrderSnapshot;
using moex::plaza2::private_state::OwnTradeSnapshot;
using moex::plaza2::private_state::Plaza2PrivateStateProjector;
using moex::plaza2::private_state::PositionScope;
using moex::plaza2::private_state::PositionSnapshot;
using moex::plaza2::private_state::ResumeMarkersSnapshot;
using moex::plaza2::private_state::StreamHealthSnapshot;
using moex::plaza2::private_state::TradingSessionSnapshot;
using moex::plaza2_twime_reconciler::MatchMode;
using moex::plaza2_twime_reconciler::OrderStatus;
using moex::plaza2_twime_reconciler::Plaza2TwimeReconciler;
using moex::plaza2_twime_reconciler::Plaza2TwimeReconcilerHealthSnapshot;
using moex::plaza2_twime_reconciler::ReconciledOrderSnapshot;
using moex::plaza2_twime_reconciler::ReconciledTradeSnapshot;
using moex::plaza2_twime_reconciler::ReconciliationSource;
using moex::plaza2_twime_reconciler::Side;
using moex::plaza2_twime_reconciler::TradeStatus;
using moex::plaza2_twime_reconciler::TwimeOrderInputKind;
using moex::plaza2_twime_reconciler::TwimeTradeInputKind;

template <typename Summary> MoexResult prepare_summary_output(Summary* out_summary) {
    if (out_summary == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (out_summary->struct_size != 0U && out_summary->struct_size < sizeof(Summary)) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    std::memset(out_summary, 0, sizeof(*out_summary));
    out_summary->struct_size = static_cast<std::uint32_t>(sizeof(Summary));
    out_summary->abi_version = MOEX_C_ABI_VERSION;
    return MOEX_RESULT_OK;
}

template <typename Item> void prepare_item_output(Item& item) {
    std::memset(&item, 0, sizeof(item));
    item.struct_size = static_cast<std::uint32_t>(sizeof(Item));
    item.abi_version = MOEX_C_ABI_VERSION;
}

MoexResult size_to_count(std::size_t size, std::uint32_t* out_count) {
    if (out_count == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (size > static_cast<std::size_t>(UINT32_MAX)) {
        *out_count = 0U;
        return MOEX_RESULT_OVERFLOW;
    }
    *out_count = static_cast<std::uint32_t>(size);
    return MOEX_RESULT_OK;
}

template <typename Native, typename Abi, typename FillFn>
MoexResult copy_snapshot_items(std::span<const Native> items, Abi* buffer, std::uint32_t capacity,
                               std::uint32_t* written, FillFn&& fill_item) {
    const auto count_result = size_to_count(items.size(), written);
    if (count_result != MOEX_RESULT_OK) {
        return count_result;
    }

    if (*written > capacity) {
        return MOEX_RESULT_BUFFER_TOO_SMALL;
    }
    if (*written > 0U && buffer == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }

    for (std::uint32_t index = 0; index < *written; ++index) {
        prepare_item_output(buffer[index]);
        fill_item(items[index], buffer[index]);
    }
    return MOEX_RESULT_OK;
}

std::uint8_t to_abi_instrument_kind(InstrumentKind kind) {
    switch (kind) {
    case InstrumentKind::kFuture:
        return MOEX_PLAZA2_INSTRUMENT_KIND_FUTURE;
    case InstrumentKind::kOption:
        return MOEX_PLAZA2_INSTRUMENT_KIND_OPTION;
    case InstrumentKind::kMultileg:
        return MOEX_PLAZA2_INSTRUMENT_KIND_MULTILEG;
    case InstrumentKind::kUnknown:
    default:
        return MOEX_PLAZA2_INSTRUMENT_KIND_UNKNOWN;
    }
}

std::uint8_t to_abi_position_scope(PositionScope scope) {
    switch (scope) {
    case PositionScope::kSettlementAccount:
        return MOEX_PLAZA2_POSITION_SCOPE_SETTLEMENT_ACCOUNT;
    case PositionScope::kClient:
    default:
        return MOEX_PLAZA2_POSITION_SCOPE_CLIENT;
    }
}

std::uint8_t to_abi_side(Side side) {
    switch (side) {
    case Side::Buy:
        return MOEX_PLAZA2_SIDE_BUY;
    case Side::Sell:
        return MOEX_PLAZA2_SIDE_SELL;
    case Side::Unknown:
    default:
        return MOEX_PLAZA2_SIDE_UNKNOWN;
    }
}

std::uint8_t to_abi_source(ReconciliationSource source) {
    switch (source) {
    case ReconciliationSource::Twime:
        return MOEX_PLAZA2_RECONCILIATION_SOURCE_TWIME;
    case ReconciliationSource::Plaza:
        return MOEX_PLAZA2_RECONCILIATION_SOURCE_PLAZA;
    case ReconciliationSource::Unknown:
    default:
        return MOEX_PLAZA2_RECONCILIATION_SOURCE_UNKNOWN;
    }
}

std::uint8_t to_abi_match_mode(MatchMode mode) {
    switch (mode) {
    case MatchMode::DirectIdentifier:
        return MOEX_PLAZA2_MATCH_MODE_DIRECT_IDENTIFIER;
    case MatchMode::ExactFallbackTuple:
        return MOEX_PLAZA2_MATCH_MODE_EXACT_FALLBACK_TUPLE;
    case MatchMode::AmbiguousCandidates:
        return MOEX_PLAZA2_MATCH_MODE_AMBIGUOUS_CANDIDATES;
    case MatchMode::None:
    default:
        return MOEX_PLAZA2_MATCH_MODE_NONE;
    }
}

std::uint8_t to_abi_order_status(OrderStatus status) {
    switch (status) {
    case OrderStatus::ProvisionalTwime:
        return MOEX_PLAZA2_ORDER_STATUS_PROVISIONAL_TWIME;
    case OrderStatus::Confirmed:
        return MOEX_PLAZA2_ORDER_STATUS_CONFIRMED;
    case OrderStatus::Rejected:
        return MOEX_PLAZA2_ORDER_STATUS_REJECTED;
    case OrderStatus::Canceled:
        return MOEX_PLAZA2_ORDER_STATUS_CANCELED;
    case OrderStatus::Filled:
        return MOEX_PLAZA2_ORDER_STATUS_FILLED;
    case OrderStatus::Ambiguous:
        return MOEX_PLAZA2_ORDER_STATUS_AMBIGUOUS;
    case OrderStatus::Diverged:
        return MOEX_PLAZA2_ORDER_STATUS_DIVERGED;
    case OrderStatus::Stale:
        return MOEX_PLAZA2_ORDER_STATUS_STALE;
    case OrderStatus::Unknown:
    default:
        return MOEX_PLAZA2_ORDER_STATUS_UNKNOWN;
    }
}

std::uint8_t to_abi_trade_status(TradeStatus status) {
    switch (status) {
    case TradeStatus::PlazaOnly:
        return MOEX_PLAZA2_TRADE_STATUS_PLAZA_ONLY;
    case TradeStatus::Matched:
        return MOEX_PLAZA2_TRADE_STATUS_MATCHED;
    case TradeStatus::Diverged:
        return MOEX_PLAZA2_TRADE_STATUS_DIVERGED;
    case TradeStatus::Ambiguous:
        return MOEX_PLAZA2_TRADE_STATUS_AMBIGUOUS;
    case TradeStatus::TwimeOnly:
    default:
        return MOEX_PLAZA2_TRADE_STATUS_TWIME_ONLY;
    }
}

std::uint8_t to_abi_twime_order_kind(TwimeOrderInputKind kind) {
    switch (kind) {
    case TwimeOrderInputKind::NewAccepted:
        return MOEX_PLAZA2_TWIME_ORDER_KIND_NEW_ACCEPTED;
    case TwimeOrderInputKind::ReplaceIntent:
        return MOEX_PLAZA2_TWIME_ORDER_KIND_REPLACE_INTENT;
    case TwimeOrderInputKind::ReplaceAccepted:
        return MOEX_PLAZA2_TWIME_ORDER_KIND_REPLACE_ACCEPTED;
    case TwimeOrderInputKind::CancelIntent:
        return MOEX_PLAZA2_TWIME_ORDER_KIND_CANCEL_INTENT;
    case TwimeOrderInputKind::CancelAccepted:
        return MOEX_PLAZA2_TWIME_ORDER_KIND_CANCEL_ACCEPTED;
    case TwimeOrderInputKind::Rejected:
        return MOEX_PLAZA2_TWIME_ORDER_KIND_REJECTED;
    case TwimeOrderInputKind::NewIntent:
    default:
        return MOEX_PLAZA2_TWIME_ORDER_KIND_NEW_INTENT;
    }
}

std::uint8_t to_abi_twime_trade_kind(TwimeTradeInputKind kind) {
    (void)kind;
    return MOEX_PLAZA2_TWIME_TRADE_KIND_EXECUTION;
}

void fill_private_connector_health(const ConnectorHealthSnapshot& native, MoexPlaza2PrivateConnectorHealth& abi) {
    abi.open = to_abi_bool(native.open);
    abi.closed = to_abi_bool(native.closed);
    abi.snapshot_active = to_abi_bool(native.snapshot_active);
    abi.online = to_abi_bool(native.online);
    abi.transaction_open = to_abi_bool(native.transaction_open);
    abi.commit_count = native.commit_count;
    abi.callback_error_count = native.callback_error_count;
}

void fill_resume_markers(const ResumeMarkersSnapshot& native, MoexPlaza2ResumeMarkers& abi) {
    abi.has_lifenum = to_abi_bool(native.has_lifenum);
    abi.last_lifenum = native.last_lifenum;
    copy_fixed(native.last_replstate, abi.last_replstate, MOEX_REPLSTATE_CAPACITY);
}

void fill_stream_health_item(const StreamHealthSnapshot& native, MoexPlaza2StreamHealthItem& abi) {
    abi.stream_code = static_cast<std::uint32_t>(native.stream_code);
    abi.online = to_abi_bool(native.online);
    abi.snapshot_complete = to_abi_bool(native.snapshot_complete);
    abi.has_publication_state = to_abi_bool(native.has_publication_state);
    abi.publication_state = native.publication_state;
    abi.last_event_type = native.last_event_type;
    abi.clear_deleted_count = native.clear_deleted_count;
    abi.committed_row_count = native.committed_row_count;
    abi.last_commit_sequence = native.last_commit_sequence;
    abi.last_trades_rev = native.last_trades_rev;
    abi.last_trades_lifenum = native.last_trades_lifenum;
    abi.last_server_time = native.last_server_time;
    abi.last_info_moment = native.last_info_moment;
    abi.last_event_id = native.last_event_id;
    copy_fixed(native.stream_name, abi.stream_name, MOEX_GROUP_CAPACITY);
    copy_fixed(native.last_message, abi.last_message, MOEX_INFO_CAPACITY);
}

void fill_trading_session_item(const TradingSessionSnapshot& native, MoexPlaza2TradingSessionItem& abi) {
    abi.sess_id = native.sess_id;
    abi.state = native.state;
    abi.inter_cl_state = native.inter_cl_state;
    abi.eve_on = to_abi_bool(native.eve_on);
    abi.mon_on = to_abi_bool(native.mon_on);
    abi.begin = native.begin;
    abi.end = native.end;
    abi.inter_cl_begin = native.inter_cl_begin;
    abi.inter_cl_end = native.inter_cl_end;
    abi.eve_begin = native.eve_begin;
    abi.eve_end = native.eve_end;
    abi.mon_begin = native.mon_begin;
    abi.mon_end = native.mon_end;
    abi.settl_sess_begin = native.settl_sess_begin;
    abi.clr_sess_begin = native.clr_sess_begin;
    abi.settl_price_calc_time = native.settl_price_calc_time;
    abi.settl_sess_t1_begin = native.settl_sess_t1_begin;
    abi.margin_call_fix_schedule = native.margin_call_fix_schedule;
}

void fill_instrument_item(const InstrumentSnapshot& native, MoexPlaza2InstrumentItem& abi) {
    abi.isin_id = native.isin_id;
    abi.sess_id = native.sess_id;
    abi.kind = to_abi_instrument_kind(native.kind);
    abi.put = to_abi_bool(native.put);
    abi.is_spread = to_abi_bool(native.is_spread);
    abi.leg_count = static_cast<std::uint8_t>(std::min<std::size_t>(native.legs.size(), UINT8_MAX));
    abi.fut_isin_id = native.fut_isin_id;
    abi.option_series_id = native.option_series_id;
    abi.inst_term = native.inst_term;
    abi.roundto = native.roundto;
    abi.lot_volume = native.lot_volume;
    abi.trade_mode_id = native.trade_mode_id;
    abi.state = native.state;
    abi.signs = native.signs;
    abi.last_trade_date = native.last_trade_date;
    abi.group_mask = native.group_mask;
    abi.trade_period_access = native.trade_period_access;
    if (!native.legs.empty()) {
        abi.leg1_isin_id = native.legs[0].leg_isin_id;
        abi.leg1_qty_ratio = native.legs[0].qty_ratio;
        abi.leg1_order_no = native.legs[0].leg_order_no;
    }
    if (native.legs.size() > 1U) {
        abi.leg2_isin_id = native.legs[1].leg_isin_id;
        abi.leg2_qty_ratio = native.legs[1].qty_ratio;
        abi.leg2_order_no = native.legs[1].leg_order_no;
    }
    copy_fixed(native.isin, abi.isin, MOEX_SYMBOL_CAPACITY);
    copy_fixed(native.short_isin, abi.short_isin, MOEX_SYMBOL_CAPACITY);
    copy_fixed(native.name, abi.name, MOEX_NAME_CAPACITY);
    copy_fixed(native.base_contract_code, abi.base_contract_code, MOEX_SYMBOL_CAPACITY);
    copy_fixed(native.min_step, abi.min_step, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.step_price, abi.step_price, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.settlement_price, abi.settlement_price, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.strike, abi.strike, MOEX_PRICE_TEXT_CAPACITY);
}

void fill_matching_map_item(const MatchingMapSnapshot& native, MoexPlaza2MatchingMapItem& abi) {
    abi.base_contract_id = native.base_contract_id;
    abi.matching_id = native.matching_id;
}

void fill_limit_item(const LimitSnapshot& native, MoexPlaza2LimitItem& abi) {
    abi.scope = to_abi_position_scope(native.scope);
    abi.limits_set = to_abi_bool(native.limits_set);
    abi.is_auto_update_limit = to_abi_bool(native.is_auto_update_limit);
    copy_fixed(native.account_code, abi.account_code, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.money_free, abi.money_free, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.money_blocked, abi.money_blocked, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.vm_reserve, abi.vm_reserve, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.fee, abi.fee, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.money_old, abi.money_old, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.money_amount, abi.money_amount, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.money_pledge_amount, abi.money_pledge_amount, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.actual_amount_of_base_currency, abi.actual_amount_of_base_currency, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.vm_intercl, abi.vm_intercl, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.broker_fee, abi.broker_fee, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.penalty, abi.penalty, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.premium_intercl, abi.premium_intercl, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.net_option_value, abi.net_option_value, MOEX_PRICE_TEXT_CAPACITY);
}

void fill_position_item(const PositionSnapshot& native, MoexPlaza2PositionItem& abi) {
    abi.scope = to_abi_position_scope(native.scope);
    abi.account_type = native.account_type;
    abi.isin_id = native.isin_id;
    abi.xpos = native.xpos;
    abi.xbuys_qty = native.xbuys_qty;
    abi.xsells_qty = native.xsells_qty;
    abi.xday_open_qty = native.xday_open_qty;
    abi.xday_open_buys_qty = native.xday_open_buys_qty;
    abi.xday_open_sells_qty = native.xday_open_sells_qty;
    abi.xopen_qty = native.xopen_qty;
    abi.last_deal_id = native.last_deal_id;
    abi.last_quantity = native.last_quantity;
    copy_fixed(native.account_code, abi.account_code, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.waprice, abi.waprice, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.net_volume_rur, abi.net_volume_rur, MOEX_PRICE_TEXT_CAPACITY);
}

void fill_own_order_item(const OwnOrderSnapshot& native, MoexPlaza2OwnOrderItem& abi) {
    abi.multileg = to_abi_bool(native.multileg);
    abi.dir = native.dir;
    abi.public_action = native.public_action;
    abi.private_action = native.private_action;
    abi.sess_id = native.sess_id;
    abi.isin_id = native.isin_id;
    abi.ext_id = native.ext_id;
    abi.from_trade_repl = to_abi_bool(native.from_trade_repl);
    abi.from_user_book = to_abi_bool(native.from_user_book);
    abi.from_current_day = to_abi_bool(native.from_current_day);
    abi.public_order_id = native.public_order_id;
    abi.private_order_id = native.private_order_id;
    abi.public_amount = native.public_amount;
    abi.public_amount_rest = native.public_amount_rest;
    abi.private_amount = native.private_amount;
    abi.private_amount_rest = native.private_amount_rest;
    abi.id_deal = native.id_deal;
    abi.xstatus = native.xstatus;
    abi.xstatus2 = native.xstatus2;
    abi.moment = native.moment;
    abi.moment_ns = native.moment_ns;
    copy_fixed(native.client_code, abi.client_code, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.login_from, abi.login_from, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.comment, abi.comment, MOEX_TEXT_CAPACITY);
    copy_fixed(native.price, abi.price_text, MOEX_PRICE_TEXT_CAPACITY);
}

void fill_own_trade_item(const OwnTradeSnapshot& native, MoexPlaza2OwnTradeItem& abi) {
    abi.multileg = to_abi_bool(native.multileg);
    abi.id_deal = native.id_deal;
    abi.sess_id = native.sess_id;
    abi.isin_id = native.isin_id;
    abi.amount = native.amount;
    abi.public_order_id_buy = native.public_order_id_buy;
    abi.public_order_id_sell = native.public_order_id_sell;
    abi.private_order_id_buy = native.private_order_id_buy;
    abi.private_order_id_sell = native.private_order_id_sell;
    abi.moment = native.moment;
    abi.moment_ns = native.moment_ns;
    copy_fixed(native.price, abi.price_text, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.rate_price, abi.rate_price, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.swap_price, abi.swap_price, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.code_buy, abi.code_buy, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.code_sell, abi.code_sell, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.comment_buy, abi.comment_buy, MOEX_TEXT_CAPACITY);
    copy_fixed(native.comment_sell, abi.comment_sell, MOEX_TEXT_CAPACITY);
    copy_fixed(native.login_buy, abi.login_buy, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.login_sell, abi.login_sell, MOEX_ACCOUNT_CAPACITY);
}

void fill_reconciler_health(const Plaza2TwimeReconcilerHealthSnapshot& native, MoexPlaza2TwimeReconcilerHealth& abi) {
    abi.logical_step = native.logical_step;
    abi.total_provisional_orders = native.total_provisional_orders;
    abi.total_confirmed_orders = native.total_confirmed_orders;
    abi.total_rejected_orders = native.total_rejected_orders;
    abi.total_canceled_orders = native.total_canceled_orders;
    abi.total_filled_orders = native.total_filled_orders;
    abi.total_diverged_orders = native.total_diverged_orders;
    abi.total_ambiguous_orders = native.total_ambiguous_orders;
    abi.total_stale_provisional_orders = native.total_stale_provisional_orders;
    abi.total_unmatched_twime_orders = native.total_unmatched_twime_orders;
    abi.total_unmatched_plaza_orders = native.total_unmatched_plaza_orders;
    abi.total_matched_trades = native.total_matched_trades;
    abi.total_diverged_trades = native.total_diverged_trades;
    abi.total_ambiguous_trades = native.total_ambiguous_trades;
    abi.total_unmatched_twime_trades = native.total_unmatched_twime_trades;
    abi.total_unmatched_plaza_trades = native.total_unmatched_plaza_trades;
    abi.plaza_revalidation_pending_orders = native.plaza_revalidation_pending_orders;
    abi.plaza_revalidation_pending_trades = native.plaza_revalidation_pending_trades;
    abi.twime_present = to_abi_bool(native.twime.present);
    abi.twime_session_state = static_cast<std::uint32_t>(native.twime.session_state);
    abi.twime_transport_open = to_abi_bool(native.twime.transport_open);
    abi.twime_session_active = to_abi_bool(native.twime.session_active);
    abi.twime_reject_seen = to_abi_bool(native.twime.reject_seen);
    abi.twime_last_reject_code = native.twime.last_reject_code;
    abi.twime_next_expected_inbound_seq = native.twime.next_expected_inbound_seq;
    abi.twime_next_outbound_seq = native.twime.next_outbound_seq;
    abi.twime_reconnect_attempts = native.twime.reconnect_attempts;
    abi.twime_faults = native.twime.faults;
    abi.twime_remote_closes = native.twime.remote_closes;
    abi.twime_last_transition_time_ms = native.twime.last_transition_time_ms;
    abi.plaza_present = to_abi_bool(native.plaza.present);
    abi.plaza_connector_open = to_abi_bool(native.plaza.connector_open);
    abi.plaza_connector_online = to_abi_bool(native.plaza.connector_online);
    abi.plaza_snapshot_active = to_abi_bool(native.plaza.snapshot_active);
    abi.plaza_required_private_streams_ready = to_abi_bool(native.plaza.required_private_streams_ready);
    abi.plaza_invalidated = to_abi_bool(native.plaza.invalidated);
    abi.plaza_last_lifenum = native.plaza.last_lifenum;
    abi.plaza_required_stream_count = native.plaza.required_stream_count;
    abi.plaza_online_stream_count = native.plaza.online_stream_count;
    abi.plaza_snapshot_complete_stream_count = native.plaza.snapshot_complete_stream_count;
    copy_fixed(native.plaza.last_replstate, abi.plaza_last_replstate, MOEX_REPLSTATE_CAPACITY);
    copy_fixed(native.plaza.last_invalidation_reason, abi.plaza_last_invalidation_reason, MOEX_REASON_CAPACITY);
}

void fill_reconciled_order_item(const ReconciledOrderSnapshot& native, MoexPlaza2ReconciledOrderItem& abi) {
    abi.status = to_abi_order_status(native.status);
    abi.match_mode = to_abi_match_mode(native.match_mode);
    abi.last_update_source = to_abi_source(native.last_update_source);
    abi.plaza_revalidation_required = to_abi_bool(native.plaza_revalidation_required);
    abi.last_update_logical_sequence = native.last_update_logical_sequence;
    abi.last_update_logical_step = native.last_update_logical_step;
    abi.twime_present = to_abi_bool(native.twime.present);
    abi.twime_last_kind = to_abi_twime_order_kind(native.twime.last_kind);
    abi.twime_side = to_abi_side(native.twime.side);
    abi.twime_multileg = to_abi_bool(native.twime.multileg);
    abi.twime_has_price = to_abi_bool(native.twime.has_price);
    abi.twime_has_order_qty = to_abi_bool(native.twime.has_order_qty);
    abi.twime_terminal_reject = to_abi_bool(native.twime.terminal_reject);
    abi.twime_terminal_cancel = to_abi_bool(native.twime.terminal_cancel);
    abi.twime_cl_ord_id = native.twime.cl_ord_id;
    abi.twime_order_id = native.twime.order_id;
    abi.twime_prev_order_id = native.twime.prev_order_id;
    abi.twime_trading_session_id = native.twime.trading_session_id;
    abi.twime_security_id = native.twime.security_id;
    abi.twime_cl_ord_link_id = native.twime.cl_ord_link_id;
    abi.twime_price_mantissa = native.twime.price_mantissa;
    abi.twime_order_qty = native.twime.order_qty;
    abi.twime_reject_code = native.twime.reject_code;
    abi.twime_last_logical_sequence = native.twime.last_logical_sequence;
    abi.twime_last_logical_step = native.twime.last_logical_step;
    abi.plaza_present = to_abi_bool(native.plaza.present);
    abi.plaza_multileg = to_abi_bool(native.plaza.multileg);
    abi.plaza_side = to_abi_side(native.plaza.side);
    abi.plaza_public_order_id = native.plaza.public_order_id;
    abi.plaza_private_order_id = native.plaza.private_order_id;
    abi.plaza_sess_id = native.plaza.sess_id;
    abi.plaza_isin_id = native.plaza.isin_id;
    abi.plaza_price_mantissa = native.plaza.price_mantissa;
    abi.plaza_public_amount = native.plaza.public_amount;
    abi.plaza_public_amount_rest = native.plaza.public_amount_rest;
    abi.plaza_private_amount = native.plaza.private_amount;
    abi.plaza_private_amount_rest = native.plaza.private_amount_rest;
    abi.plaza_id_deal = native.plaza.id_deal;
    abi.plaza_xstatus = native.plaza.xstatus;
    abi.plaza_xstatus2 = native.plaza.xstatus2;
    abi.plaza_ext_id = native.plaza.ext_id;
    abi.plaza_public_action = native.plaza.public_action;
    abi.plaza_private_action = native.plaza.private_action;
    abi.plaza_from_trade_repl = to_abi_bool(native.plaza.from_trade_repl);
    abi.plaza_from_user_book = to_abi_bool(native.plaza.from_user_book);
    abi.plaza_from_current_day = to_abi_bool(native.plaza.from_current_day);
    abi.plaza_moment = native.plaza.moment;
    abi.plaza_moment_ns = native.plaza.moment_ns;
    abi.plaza_last_logical_sequence = native.plaza.last_logical_sequence;
    copy_fixed(native.fault_reason, abi.fault_reason, MOEX_REASON_CAPACITY);
    copy_fixed(native.twime.account, abi.twime_account, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.twime.compliance_id, abi.twime_compliance_id, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.plaza.client_code, abi.plaza_client_code, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.plaza.login_from, abi.plaza_login_from, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.plaza.comment, abi.plaza_comment, MOEX_TEXT_CAPACITY);
    copy_fixed(native.plaza.price_text, abi.plaza_price_text, MOEX_PRICE_TEXT_CAPACITY);
}

void fill_reconciled_trade_item(const ReconciledTradeSnapshot& native, MoexPlaza2ReconciledTradeItem& abi) {
    abi.status = to_abi_trade_status(native.status);
    abi.match_mode = to_abi_match_mode(native.match_mode);
    abi.last_update_source = to_abi_source(native.last_update_source);
    abi.plaza_revalidation_required = to_abi_bool(native.plaza_revalidation_required);
    abi.last_update_logical_sequence = native.last_update_logical_sequence;
    abi.last_update_logical_step = native.last_update_logical_step;
    abi.twime_present = to_abi_bool(native.twime.present);
    abi.twime_last_kind = to_abi_twime_trade_kind(native.twime.last_kind);
    abi.twime_side = to_abi_side(native.twime.side);
    abi.twime_multileg = to_abi_bool(native.twime.multileg);
    abi.twime_has_price = to_abi_bool(native.twime.has_price);
    abi.twime_has_last_qty = to_abi_bool(native.twime.has_last_qty);
    abi.twime_has_order_qty = to_abi_bool(native.twime.has_order_qty);
    abi.twime_cl_ord_id = native.twime.cl_ord_id;
    abi.twime_order_id = native.twime.order_id;
    abi.twime_trade_id = native.twime.trade_id;
    abi.twime_trading_session_id = native.twime.trading_session_id;
    abi.twime_security_id = native.twime.security_id;
    abi.twime_price_mantissa = native.twime.price_mantissa;
    abi.twime_last_qty = native.twime.last_qty;
    abi.twime_order_qty = native.twime.order_qty;
    abi.twime_last_logical_sequence = native.twime.last_logical_sequence;
    abi.twime_last_logical_step = native.twime.last_logical_step;
    abi.plaza_present = to_abi_bool(native.plaza.present);
    abi.plaza_multileg = to_abi_bool(native.plaza.multileg);
    abi.plaza_trade_id = native.plaza.trade_id;
    abi.plaza_sess_id = native.plaza.sess_id;
    abi.plaza_isin_id = native.plaza.isin_id;
    abi.plaza_price_mantissa = native.plaza.price_mantissa;
    abi.plaza_amount = native.plaza.amount;
    abi.plaza_public_order_id_buy = native.plaza.public_order_id_buy;
    abi.plaza_public_order_id_sell = native.plaza.public_order_id_sell;
    abi.plaza_private_order_id_buy = native.plaza.private_order_id_buy;
    abi.plaza_private_order_id_sell = native.plaza.private_order_id_sell;
    abi.plaza_moment = native.plaza.moment;
    abi.plaza_moment_ns = native.plaza.moment_ns;
    abi.plaza_last_logical_sequence = native.plaza.last_logical_sequence;
    copy_fixed(native.fault_reason, abi.fault_reason, MOEX_REASON_CAPACITY);
    copy_fixed(native.plaza.price_text, abi.plaza_price_text, MOEX_PRICE_TEXT_CAPACITY);
    copy_fixed(native.plaza.code_buy, abi.plaza_code_buy, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.plaza.code_sell, abi.plaza_code_sell, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.plaza.comment_buy, abi.plaza_comment_buy, MOEX_TEXT_CAPACITY);
    copy_fixed(native.plaza.comment_sell, abi.plaza_comment_sell, MOEX_TEXT_CAPACITY);
    copy_fixed(native.plaza.login_buy, abi.plaza_login_buy, MOEX_ACCOUNT_CAPACITY);
    copy_fixed(native.plaza.login_sell, abi.plaza_login_sell, MOEX_ACCOUNT_CAPACITY);
}

} // namespace

void dispatch_low_rate_callback(MoexHandleTag* handle, const MoexPolledEvent& event) {
    if (handle == nullptr || handle->low_rate_callback == nullptr) {
        return;
    }
    handle->low_rate_callback(&event.header, &event, handle->low_rate_user_data);
}

extern "C" {

const char* moex_phase0_abi_name(void) {
    return "moex_phase0_abi";
}

uint32_t moex_phase0_abi_version(void) {
    return MOEX_C_ABI_VERSION;
}

uint8_t moex_environment_start_allowed(const char* environment, uint8_t armed) {
    const std::string_view env = environment == nullptr ? std::string_view{} : std::string_view(environment);
    return to_abi_bool(moex::phase0::prod_requires_arm(env, armed != 0U));
}

uint8_t moex_prod_requires_explicit_arm(void) {
    return 1U;
}

bool moex_phase0_prod_requires_arm(const char* environment, bool armed) {
    return moex_environment_start_allowed(environment, to_abi_bool(armed)) != 0U;
}

uint32_t moex_sizeof_event_header(void) {
    return static_cast<uint32_t>(sizeof(MoexEventHeader));
}

uint32_t moex_sizeof_backpressure_counters(void) {
    return static_cast<uint32_t>(sizeof(MoexBackpressureCounters));
}

uint32_t moex_sizeof_health_snapshot(void) {
    return static_cast<uint32_t>(sizeof(MoexHealthSnapshot));
}

uint32_t moex_sizeof_connector_create_params(void) {
    return static_cast<uint32_t>(sizeof(MoexConnectorCreateParams));
}

uint32_t moex_sizeof_profile_load_params(void) {
    return static_cast<uint32_t>(sizeof(MoexProfileLoadParams));
}

uint32_t moex_sizeof_order_submit_request(void) {
    return static_cast<uint32_t>(sizeof(MoexOrderSubmitRequest));
}

uint32_t moex_sizeof_order_cancel_request(void) {
    return static_cast<uint32_t>(sizeof(MoexOrderCancelRequest));
}

uint32_t moex_sizeof_order_replace_request(void) {
    return static_cast<uint32_t>(sizeof(MoexOrderReplaceRequest));
}

uint32_t moex_sizeof_mass_cancel_request(void) {
    return static_cast<uint32_t>(sizeof(MoexMassCancelRequest));
}

uint32_t moex_sizeof_subscription_request(void) {
    return static_cast<uint32_t>(sizeof(MoexSubscriptionRequest));
}

uint32_t moex_sizeof_polled_event(void) {
    return static_cast<uint32_t>(sizeof(MoexPolledEvent));
}

uint32_t moex_sizeof_plaza2_private_connector_health(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2PrivateConnectorHealth));
}

uint32_t moex_sizeof_plaza2_resume_markers(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2ResumeMarkers));
}

uint32_t moex_sizeof_plaza2_stream_health_item(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2StreamHealthItem));
}

uint32_t moex_sizeof_plaza2_trading_session_item(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2TradingSessionItem));
}

uint32_t moex_sizeof_plaza2_instrument_item(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2InstrumentItem));
}

uint32_t moex_sizeof_plaza2_matching_map_item(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2MatchingMapItem));
}

uint32_t moex_sizeof_plaza2_limit_item(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2LimitItem));
}

uint32_t moex_sizeof_plaza2_position_item(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2PositionItem));
}

uint32_t moex_sizeof_plaza2_own_order_item(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2OwnOrderItem));
}

uint32_t moex_sizeof_plaza2_own_trade_item(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2OwnTradeItem));
}

uint32_t moex_sizeof_plaza2_twime_reconciler_health(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2TwimeReconcilerHealth));
}

uint32_t moex_sizeof_plaza2_reconciled_order_item(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2ReconciledOrderItem));
}

uint32_t moex_sizeof_plaza2_reconciled_trade_item(void) {
    return static_cast<uint32_t>(sizeof(MoexPlaza2ReconciledTradeItem));
}

uint32_t moex_alignof_event_header(void) {
    return static_cast<uint32_t>(alignof(MoexEventHeader));
}

uint32_t moex_alignof_backpressure_counters(void) {
    return static_cast<uint32_t>(alignof(MoexBackpressureCounters));
}

uint32_t moex_alignof_health_snapshot(void) {
    return static_cast<uint32_t>(alignof(MoexHealthSnapshot));
}

uint32_t moex_alignof_connector_create_params(void) {
    return static_cast<uint32_t>(alignof(MoexConnectorCreateParams));
}

uint32_t moex_alignof_profile_load_params(void) {
    return static_cast<uint32_t>(alignof(MoexProfileLoadParams));
}

uint32_t moex_alignof_order_submit_request(void) {
    return static_cast<uint32_t>(alignof(MoexOrderSubmitRequest));
}

uint32_t moex_alignof_order_cancel_request(void) {
    return static_cast<uint32_t>(alignof(MoexOrderCancelRequest));
}

uint32_t moex_alignof_order_replace_request(void) {
    return static_cast<uint32_t>(alignof(MoexOrderReplaceRequest));
}

uint32_t moex_alignof_mass_cancel_request(void) {
    return static_cast<uint32_t>(alignof(MoexMassCancelRequest));
}

uint32_t moex_alignof_subscription_request(void) {
    return static_cast<uint32_t>(alignof(MoexSubscriptionRequest));
}

uint32_t moex_alignof_polled_event(void) {
    return static_cast<uint32_t>(alignof(MoexPolledEvent));
}

uint32_t moex_alignof_plaza2_private_connector_health(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2PrivateConnectorHealth));
}

uint32_t moex_alignof_plaza2_resume_markers(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2ResumeMarkers));
}

uint32_t moex_alignof_plaza2_stream_health_item(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2StreamHealthItem));
}

uint32_t moex_alignof_plaza2_trading_session_item(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2TradingSessionItem));
}

uint32_t moex_alignof_plaza2_instrument_item(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2InstrumentItem));
}

uint32_t moex_alignof_plaza2_matching_map_item(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2MatchingMapItem));
}

uint32_t moex_alignof_plaza2_limit_item(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2LimitItem));
}

uint32_t moex_alignof_plaza2_position_item(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2PositionItem));
}

uint32_t moex_alignof_plaza2_own_order_item(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2OwnOrderItem));
}

uint32_t moex_alignof_plaza2_own_trade_item(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2OwnTradeItem));
}

uint32_t moex_alignof_plaza2_twime_reconciler_health(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2TwimeReconcilerHealth));
}

uint32_t moex_alignof_plaza2_reconciled_order_item(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2ReconciledOrderItem));
}

uint32_t moex_alignof_plaza2_reconciled_trade_item(void) {
    return static_cast<uint32_t>(alignof(MoexPlaza2ReconciledTradeItem));
}

MoexResult moex_create_connector(const MoexConnectorCreateParams* params, MoexConnectorHandle* out_handle) {
    if (params == nullptr || out_handle == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    if (params->struct_size < sizeof(MoexConnectorCreateParams) || params->abi_version != MOEX_C_ABI_VERSION) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }

    try {
        auto handle = std::make_unique<MoexHandleTag>();
        handle->connector_name = params->connector_name == nullptr ? "" : params->connector_name;
        handle->instance_id = params->instance_id == nullptr ? "" : params->instance_id;
        const auto seed = moex::phase0::make_seed_counters(moex::phase0::EventLossPolicy::Lossless);
        handle->counters.produced = seed.produced;
        handle->counters.polled = seed.polled;
        handle->counters.dropped = seed.dropped;
        handle->counters.high_watermark = seed.high_watermark;
        handle->counters.overflowed = to_abi_bool(seed.overflowed);
        *out_handle = handle.release();
        return MOEX_RESULT_OK;
    } catch (...) {
        return MOEX_RESULT_INTERNAL_ERROR;
    }
}

MoexResult moex_destroy_connector(MoexConnectorHandle handle) {
    if (handle == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    delete handle;
    return MOEX_RESULT_OK;
}

MoexResult moex_load_profile(MoexConnectorHandle handle, const MoexProfileLoadParams* params) {
    if (handle == nullptr || params == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    if (params->struct_size < sizeof(MoexProfileLoadParams) || params->abi_version != MOEX_C_ABI_VERSION) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }

    handle->profile_loaded = true;
    handle->prod_armed = params->armed != 0;
    handle->profile_path = params->profile_path == nullptr ? "" : params->profile_path;
    return MOEX_RESULT_OK;
}

MoexResult moex_load_synthetic_replay(MoexConnectorHandle handle, const char* replay_path) {
    if (handle == nullptr || replay_path == nullptr || replay_path[0] == '\0') {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }

    std::vector<MoexPolledEvent> events;
    std::string error;
    if (!parse_replay_file(replay_path, events, error)) {
        (void)error;
        return MOEX_RESULT_INVALID_ARGUMENT;
    }

    handle->replay_path = replay_path;
    handle->replay_events = std::move(events);
    handle->replay_loaded = true;
    handle->started = false;
    handle->started_callback_emitted = false;
    handle->next_replay_index = 0;
    handle->shadow_mode_enabled = true;
    handle->counters.produced = static_cast<std::uint64_t>(handle->replay_events.size());
    handle->counters.polled = 0;
    handle->counters.dropped = 0;
    handle->counters.high_watermark = static_cast<std::uint64_t>(handle->replay_events.size());
    handle->counters.overflowed = 0U;
    return MOEX_RESULT_OK;
}

MoexResult moex_start_connector(MoexConnectorHandle handle) {
    if (handle == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    if (!handle->profile_loaded || !handle->replay_loaded) {
        return MOEX_RESULT_NOT_INITIALIZED;
    }
    if (handle->started) {
        return MOEX_RESULT_ALREADY_STARTED;
    }

    handle->started = true;
    if (!handle->started_callback_emitted) {
        dispatch_low_rate_callback(handle, make_status_event(0, "synthetic replay started", MOEX_REPLAY_STATE_STARTED));
        handle->started_callback_emitted = true;
    }
    return MOEX_RESULT_OK;
}

MoexResult moex_stop_connector(MoexConnectorHandle handle) {
    if (handle == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    if (!handle->started) {
        return MOEX_RESULT_NOT_STARTED;
    }
    handle->started = false;
    return MOEX_RESULT_OK;
}

MoexResult moex_submit_order_placeholder(MoexConnectorHandle handle, const MoexOrderSubmitRequest* request) {
    if (handle == nullptr || request == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_NOT_SUPPORTED;
}

MoexResult moex_cancel_order_placeholder(MoexConnectorHandle handle, const MoexOrderCancelRequest* request) {
    if (handle == nullptr || request == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_NOT_SUPPORTED;
}

MoexResult moex_replace_order_placeholder(MoexConnectorHandle handle, const MoexOrderReplaceRequest* request) {
    if (handle == nullptr || request == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_NOT_SUPPORTED;
}

MoexResult moex_mass_cancel_placeholder(MoexConnectorHandle handle, const MoexMassCancelRequest* request) {
    if (handle == nullptr || request == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_NOT_SUPPORTED;
}

MoexResult moex_subscribe_placeholder(MoexConnectorHandle handle, const MoexSubscriptionRequest* request) {
    if (handle == nullptr || request == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_NOT_SUPPORTED;
}

MoexResult moex_unsubscribe_placeholder(MoexConnectorHandle handle, const MoexSubscriptionRequest* request) {
    if (handle == nullptr || request == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_NOT_SUPPORTED;
}

MoexResult moex_poll_events_v2(MoexConnectorHandle handle, void* out_events, uint32_t event_stride_bytes,
                               uint32_t capacity, uint32_t* written) {
    if (handle == nullptr || written == nullptr || (capacity > 0U && out_events == nullptr)) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    if (!handle->started) {
        return MOEX_RESULT_NOT_STARTED;
    }
    if (capacity > 0U && event_stride_bytes < sizeof(MoexPolledEvent)) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }

    auto* output_bytes = static_cast<std::byte*>(out_events);
    std::uint32_t emitted = 0;
    while (emitted < capacity && handle->next_replay_index < handle->replay_events.size()) {
        auto event = handle->replay_events[handle->next_replay_index++];
        event.header.managed_poll_monotonic_ns = event.header.publish_monotonic_ns + 1'000LL;
        std::memcpy(output_bytes + (static_cast<std::size_t>(emitted) * event_stride_bytes), &event, sizeof(event));
        emitted += 1U;
        handle->counters.polled += 1U;
        if (event.header.event_type == MOEX_EVENT_REPLAY_STATE) {
            dispatch_low_rate_callback(handle, event);
        }
    }

    *written = emitted;
    return MOEX_RESULT_OK;
}

MoexResult moex_poll_events(MoexConnectorHandle handle, void* out_events, uint32_t capacity, uint32_t* written) {
    return moex_poll_events_v2(handle, out_events, static_cast<uint32_t>(sizeof(MoexPolledEvent)), capacity, written);
}

MoexResult moex_register_low_rate_callback(MoexConnectorHandle handle, MoexLowRateCallback callback, void* user_data) {
    if (handle == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    handle->low_rate_callback = callback;
    handle->low_rate_user_data = user_data;
    return MOEX_RESULT_OK;
}

MoexResult moex_get_health(MoexConnectorHandle handle, MoexHealthSnapshot* out_health) {
    if (handle == nullptr || out_health == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    if (out_health->struct_size != 0U && out_health->struct_size < sizeof(MoexHealthSnapshot)) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }

    std::memset(out_health, 0, sizeof(*out_health));
    out_health->struct_size = static_cast<std::uint32_t>(sizeof(MoexHealthSnapshot));
    out_health->abi_version = MOEX_C_ABI_VERSION;
    out_health->connector_state = handle->started ? 2U : 1U;
    out_health->active_profile_kind = handle->replay_loaded ? 2U : (handle->profile_loaded ? 1U : 0U);
    out_health->prod_armed = handle->prod_armed ? 1U : 0U;
    out_health->shadow_mode_enabled = handle->shadow_mode_enabled ? 1U : 0U;
    return MOEX_RESULT_OK;
}

MoexResult moex_get_backpressure_counters(MoexConnectorHandle handle, MoexBackpressureCounters* out_counters) {
    if (handle == nullptr || out_counters == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    *out_counters = handle->counters;
    return MOEX_RESULT_OK;
}

MoexResult moex_flush_recovery_state(MoexConnectorHandle handle) {
    if (handle == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_OK;
}

MoexResult moex_get_plaza2_private_connector_health(MoexConnectorHandle handle,
                                                    MoexPlaza2PrivateConnectorHealth* out_health) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    const auto prepare_result = prepare_summary_output(out_health);
    if (prepare_result != MOEX_RESULT_OK) {
        return prepare_result;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        fill_private_connector_health(handle->plaza2_private_state->connector_health(), *out_health);
        return MOEX_RESULT_OK;
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult moex_get_plaza2_resume_markers(MoexConnectorHandle handle, MoexPlaza2ResumeMarkers* out_markers) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    const auto prepare_result = prepare_summary_output(out_markers);
    if (prepare_result != MOEX_RESULT_OK) {
        return prepare_result;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        fill_resume_markers(handle->plaza2_private_state->resume_markers(), *out_markers);
        return MOEX_RESULT_OK;
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult moex_get_plaza2_stream_health_count(MoexConnectorHandle handle, uint32_t* out_count) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (out_count == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    return size_to_count(handle->plaza2_private_state->stream_health().size(), out_count);
}

MoexResult moex_copy_plaza2_stream_health_items(MoexConnectorHandle handle, MoexPlaza2StreamHealthItem* buffer,
                                                uint32_t capacity, uint32_t* written) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (written == nullptr || (capacity > 0U && buffer == nullptr)) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        return copy_snapshot_items(handle->plaza2_private_state->stream_health(), buffer, capacity, written,
                                   fill_stream_health_item);
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult moex_get_plaza2_trading_session_count(MoexConnectorHandle handle, uint32_t* out_count) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (out_count == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    return size_to_count(handle->plaza2_private_state->sessions().size(), out_count);
}

MoexResult moex_copy_plaza2_trading_session_items(MoexConnectorHandle handle, MoexPlaza2TradingSessionItem* buffer,
                                                  uint32_t capacity, uint32_t* written) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (written == nullptr || (capacity > 0U && buffer == nullptr)) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        return copy_snapshot_items(handle->plaza2_private_state->sessions(), buffer, capacity, written,
                                   fill_trading_session_item);
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult moex_get_plaza2_instrument_count(MoexConnectorHandle handle, uint32_t* out_count) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (out_count == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    return size_to_count(handle->plaza2_private_state->instruments().size(), out_count);
}

MoexResult moex_copy_plaza2_instrument_items(MoexConnectorHandle handle, MoexPlaza2InstrumentItem* buffer,
                                             uint32_t capacity, uint32_t* written) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (written == nullptr || (capacity > 0U && buffer == nullptr)) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        return copy_snapshot_items(handle->plaza2_private_state->instruments(), buffer, capacity, written,
                                   fill_instrument_item);
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult moex_get_plaza2_matching_map_count(MoexConnectorHandle handle, uint32_t* out_count) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (out_count == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    return size_to_count(handle->plaza2_private_state->matching_map().size(), out_count);
}

MoexResult moex_copy_plaza2_matching_map_items(MoexConnectorHandle handle, MoexPlaza2MatchingMapItem* buffer,
                                               uint32_t capacity, uint32_t* written) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (written == nullptr || (capacity > 0U && buffer == nullptr)) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        return copy_snapshot_items(handle->plaza2_private_state->matching_map(), buffer, capacity, written,
                                   fill_matching_map_item);
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult moex_get_plaza2_limit_count(MoexConnectorHandle handle, uint32_t* out_count) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (out_count == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    return size_to_count(handle->plaza2_private_state->limits().size(), out_count);
}

MoexResult moex_copy_plaza2_limit_items(MoexConnectorHandle handle, MoexPlaza2LimitItem* buffer, uint32_t capacity,
                                        uint32_t* written) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (written == nullptr || (capacity > 0U && buffer == nullptr)) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        return copy_snapshot_items(handle->plaza2_private_state->limits(), buffer, capacity, written, fill_limit_item);
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult moex_get_plaza2_position_count(MoexConnectorHandle handle, uint32_t* out_count) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (out_count == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    return size_to_count(handle->plaza2_private_state->positions().size(), out_count);
}

MoexResult moex_copy_plaza2_position_items(MoexConnectorHandle handle, MoexPlaza2PositionItem* buffer,
                                           uint32_t capacity, uint32_t* written) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (written == nullptr || (capacity > 0U && buffer == nullptr)) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        return copy_snapshot_items(handle->plaza2_private_state->positions(), buffer, capacity, written,
                                   fill_position_item);
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult moex_get_plaza2_own_order_count(MoexConnectorHandle handle, uint32_t* out_count) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (out_count == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    return size_to_count(handle->plaza2_private_state->own_orders().size(), out_count);
}

MoexResult moex_copy_plaza2_own_order_items(MoexConnectorHandle handle, MoexPlaza2OwnOrderItem* buffer,
                                            uint32_t capacity, uint32_t* written) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (written == nullptr || (capacity > 0U && buffer == nullptr)) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        return copy_snapshot_items(handle->plaza2_private_state->own_orders(), buffer, capacity, written,
                                   fill_own_order_item);
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult moex_get_plaza2_own_trade_count(MoexConnectorHandle handle, uint32_t* out_count) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (out_count == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    return size_to_count(handle->plaza2_private_state->own_trades().size(), out_count);
}

MoexResult moex_copy_plaza2_own_trade_items(MoexConnectorHandle handle, MoexPlaza2OwnTradeItem* buffer,
                                            uint32_t capacity, uint32_t* written) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (written == nullptr || (capacity > 0U && buffer == nullptr)) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_private_state == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        return copy_snapshot_items(handle->plaza2_private_state->own_trades(), buffer, capacity, written,
                                   fill_own_trade_item);
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult moex_get_plaza2_twime_reconciler_health(MoexConnectorHandle handle,
                                                   MoexPlaza2TwimeReconcilerHealth* out_health) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    const auto prepare_result = prepare_summary_output(out_health);
    if (prepare_result != MOEX_RESULT_OK) {
        return prepare_result;
    }
    if (handle->plaza2_twime_reconciler == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        fill_reconciler_health(handle->plaza2_twime_reconciler->health(), *out_health);
        return MOEX_RESULT_OK;
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult moex_get_plaza2_reconciled_order_count(MoexConnectorHandle handle, uint32_t* out_count) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (out_count == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_twime_reconciler == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    return size_to_count(handle->plaza2_twime_reconciler->orders().size(), out_count);
}

MoexResult moex_copy_plaza2_reconciled_order_items(MoexConnectorHandle handle, MoexPlaza2ReconciledOrderItem* buffer,
                                                   uint32_t capacity, uint32_t* written) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (written == nullptr || (capacity > 0U && buffer == nullptr)) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_twime_reconciler == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        return copy_snapshot_items(handle->plaza2_twime_reconciler->orders(), buffer, capacity, written,
                                   fill_reconciled_order_item);
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult moex_get_plaza2_reconciled_trade_count(MoexConnectorHandle handle, uint32_t* out_count) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (out_count == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_twime_reconciler == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    return size_to_count(handle->plaza2_twime_reconciler->trades().size(), out_count);
}

MoexResult moex_copy_plaza2_reconciled_trade_items(MoexConnectorHandle handle, MoexPlaza2ReconciledTradeItem* buffer,
                                                   uint32_t capacity, uint32_t* written) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (written == nullptr || (capacity > 0U && buffer == nullptr)) {
        return MOEX_RESULT_NULL_POINTER;
    }
    if (handle->plaza2_twime_reconciler == nullptr) {
        return MOEX_RESULT_SNAPSHOT_UNAVAILABLE;
    }
    try {
        return copy_snapshot_items(handle->plaza2_twime_reconciler->trades(), buffer, capacity, written,
                                   fill_reconciled_trade_item);
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

} // extern "C"

namespace moex::capi_internal {

MoexResult install_private_state_projector(MoexConnectorHandle handle,
                                           plaza2::private_state::Plaza2PrivateStateProjector projector) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    try {
        handle->plaza2_private_state =
            std::make_unique<plaza2::private_state::Plaza2PrivateStateProjector>(std::move(projector));
        return MOEX_RESULT_OK;
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

MoexResult install_reconciler_snapshot(MoexConnectorHandle handle,
                                       plaza2_twime_reconciler::Plaza2TwimeReconciler reconciler) {
    if (handle == nullptr) {
        return MOEX_RESULT_NULL_POINTER;
    }
    try {
        handle->plaza2_twime_reconciler =
            std::make_unique<plaza2_twime_reconciler::Plaza2TwimeReconciler>(std::move(reconciler));
        return MOEX_RESULT_OK;
    } catch (...) {
        return MOEX_RESULT_TRANSLATION_FAILED;
    }
}

} // namespace moex::capi_internal
