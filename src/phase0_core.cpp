#include "adapters/alorengine_capi/moex_c_api.h"
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
#include <string>
#include <string_view>
#include <vector>

namespace moex::phase0 {

namespace {

std::string to_lower(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

}  // namespace

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

}  // namespace moex::phase0

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

}  // namespace moex::logging

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

MoexPolledEvent make_event(
    MoexEventType event_type,
    std::uint64_t sequence,
    std::int64_t event_time_ms,
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
    auto event = make_event(
        replay_state == MOEX_REPLAY_STATE_UNSPECIFIED ? MOEX_EVENT_CONNECTOR_STATUS : MOEX_EVENT_REPLAY_STATE,
        sequence,
        0,
        true);
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

}  // namespace

struct MoexHandleTag {
    std::string connector_name;
    std::string instance_id;
    std::string profile_path;
    std::string replay_path;
    bool started = false;
    bool profile_loaded = false;
    bool prod_armed = false;
    bool shadow_mode_enabled = true;
    bool replay_loaded = false;
    bool started_callback_emitted = false;
    MoexLowRateCallback low_rate_callback = nullptr;
    void* low_rate_user_data = nullptr;
    MoexBackpressureCounters counters{};
    std::vector<MoexPolledEvent> replay_events;
    std::size_t next_replay_index = 0;
};

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

MoexResult moex_poll_events_v2(
    MoexConnectorHandle handle,
    void* out_events,
    uint32_t event_stride_bytes,
    uint32_t capacity,
    uint32_t* written) {
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

}  // extern "C"
