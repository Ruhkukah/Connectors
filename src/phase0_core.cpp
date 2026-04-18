#include "adapters/alorengine_capi/moex_c_api.h"
#include "moex_core/logging/redaction.hpp"
#include "moex_core/phase0_core.hpp"

#include <algorithm>
#include <cctype>
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

}  // extern "C"
