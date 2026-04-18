#include "adapters/alorengine_capi/moex_c_api.h"
#include "moex_core/logging/redaction.hpp"
#include "moex_core/phase0_core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>

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

bool has_valid_abi_prefix(std::uint32_t struct_size, std::uint32_t expected_size, std::uint16_t abi_version) {
    return struct_size >= expected_size && abi_version == MOEX_C_ABI_VERSION;
}

std::string_view nullable_string(const char* value) {
    return value == nullptr ? std::string_view{} : std::string_view(value);
}

MoexResult validate_handle(const MoexHandleTag* handle) {
    return handle == nullptr ? MOEX_RESULT_INVALID_ARGUMENT : MOEX_RESULT_OK;
}

}  // namespace

struct MoexHandleTag {
    std::string connector_name;
    std::string instance_id;
    bool started = false;
    bool profile_loaded = false;
    bool prod_armed = false;
    bool shadow_mode_enabled = false;
    MoexLowRateCallback low_rate_callback = nullptr;
    void* low_rate_user_data = nullptr;
    MoexBackpressureCounters counters{};
};

extern "C" {

const char* moex_phase0_abi_name(void) {
    return "moex_phase0_abi";
}

uint32_t moex_phase0_abi_version(void) {
    return MOEX_C_ABI_VERSION;
}

bool moex_phase0_prod_requires_arm(const char* environment, bool armed) {
    const std::string_view env = environment == nullptr ? std::string_view{} : std::string_view(environment);
    return moex::phase0::prod_requires_arm(env, armed);
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

MoexResult moex_create_connector(const MoexConnectorCreateParams* params, MoexConnectorHandle* out_handle) {
    if (params == nullptr || out_handle == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    if (!has_valid_abi_prefix(params->struct_size, sizeof(MoexConnectorCreateParams), params->abi_version)) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }

    try {
        auto handle = std::make_unique<MoexHandleTag>();
        handle->connector_name = std::string(nullable_string(params->connector_name));
        handle->instance_id = std::string(nullable_string(params->instance_id));
        const auto seed = moex::phase0::make_seed_counters(moex::phase0::EventLossPolicy::Lossless);
        handle->counters.produced = seed.produced;
        handle->counters.polled = seed.polled;
        handle->counters.dropped = seed.dropped;
        handle->counters.high_watermark = seed.high_watermark;
        handle->counters.overflowed = seed.overflowed;
        *out_handle = handle.release();
        return MOEX_RESULT_OK;
    } catch (const std::bad_alloc&) {
        return MOEX_RESULT_INTERNAL_ERROR;
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
    if (validate_handle(handle) != MOEX_RESULT_OK || params == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    if (!has_valid_abi_prefix(params->struct_size, sizeof(MoexProfileLoadParams), params->abi_version)) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    handle->profile_loaded = true;
    handle->prod_armed = params->armed != 0;
    return MOEX_RESULT_OK;
}

MoexResult moex_start_connector(MoexConnectorHandle handle) {
    if (validate_handle(handle) != MOEX_RESULT_OK) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    if (handle->started) {
        return MOEX_RESULT_ALREADY_STARTED;
    }
    handle->started = true;
    return MOEX_RESULT_OK;
}

MoexResult moex_stop_connector(MoexConnectorHandle handle) {
    if (validate_handle(handle) != MOEX_RESULT_OK) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    if (!handle->started) {
        return MOEX_RESULT_NOT_STARTED;
    }
    handle->started = false;
    return MOEX_RESULT_OK;
}

MoexResult moex_submit_order_placeholder(MoexConnectorHandle handle, const MoexOrderSubmitRequest* request) {
    if (validate_handle(handle) != MOEX_RESULT_OK || request == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_NOT_SUPPORTED;
}

MoexResult moex_cancel_order_placeholder(MoexConnectorHandle handle, const MoexOrderCancelRequest* request) {
    if (validate_handle(handle) != MOEX_RESULT_OK || request == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_NOT_SUPPORTED;
}

MoexResult moex_replace_order_placeholder(MoexConnectorHandle handle, const MoexOrderReplaceRequest* request) {
    if (validate_handle(handle) != MOEX_RESULT_OK || request == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_NOT_SUPPORTED;
}

MoexResult moex_mass_cancel_placeholder(MoexConnectorHandle handle, const MoexMassCancelRequest* request) {
    if (validate_handle(handle) != MOEX_RESULT_OK || request == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_NOT_SUPPORTED;
}

MoexResult moex_subscribe_placeholder(MoexConnectorHandle handle, const MoexSubscriptionRequest* request) {
    if (validate_handle(handle) != MOEX_RESULT_OK || request == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_NOT_SUPPORTED;
}

MoexResult moex_unsubscribe_placeholder(MoexConnectorHandle handle, const MoexSubscriptionRequest* request) {
    if (validate_handle(handle) != MOEX_RESULT_OK || request == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_NOT_SUPPORTED;
}

MoexResult moex_poll_events(MoexConnectorHandle handle, void* out_events, uint32_t capacity, uint32_t* written) {
    (void)out_events;
    (void)capacity;
    if (validate_handle(handle) != MOEX_RESULT_OK || written == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    *written = 0;
    return MOEX_RESULT_OK;
}

MoexResult moex_register_low_rate_callback(MoexConnectorHandle handle, MoexLowRateCallback callback, void* user_data) {
    if (validate_handle(handle) != MOEX_RESULT_OK) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    handle->low_rate_callback = callback;
    handle->low_rate_user_data = user_data;
    return MOEX_RESULT_OK;
}

MoexResult moex_get_health(MoexConnectorHandle handle, MoexHealthSnapshot* out_health) {
    if (validate_handle(handle) != MOEX_RESULT_OK || out_health == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    if (out_health->struct_size != 0 && out_health->struct_size < sizeof(MoexHealthSnapshot)) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    std::memset(out_health, 0, sizeof(*out_health));
    out_health->struct_size = static_cast<uint32_t>(sizeof(MoexHealthSnapshot));
    out_health->abi_version = MOEX_C_ABI_VERSION;
    out_health->connector_state = handle->started ? 2u : 1u;
    out_health->active_profile_kind = handle->profile_loaded ? 1u : 0u;
    out_health->prod_armed = handle->prod_armed ? 1u : 0u;
    out_health->shadow_mode_enabled = handle->shadow_mode_enabled ? 1u : 0u;
    return MOEX_RESULT_OK;
}

MoexResult moex_get_backpressure_counters(MoexConnectorHandle handle, MoexBackpressureCounters* out_counters) {
    if (validate_handle(handle) != MOEX_RESULT_OK || out_counters == nullptr) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    *out_counters = handle->counters;
    return MOEX_RESULT_OK;
}

MoexResult moex_flush_recovery_state(MoexConnectorHandle handle) {
    if (validate_handle(handle) != MOEX_RESULT_OK) {
        return MOEX_RESULT_INVALID_ARGUMENT;
    }
    return MOEX_RESULT_OK;
}

}  // extern "C"
