#include "moex/plaza2/cgate/plaza2_manual_operator_gate.hpp"

#include <algorithm>
#include <string>

namespace moex::plaza2::cgate {

namespace {

bool is_loopback_host(std::string_view host) {
    std::string normalized(host);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized == "127.0.0.1" || normalized == "::1" || normalized == "localhost";
}

Plaza2ManualOperatorGateResult success() {
    return {
        .allowed = true,
        .error_code = Plaza2ErrorCode::None,
        .reason = {},
    };
}

Plaza2ManualOperatorGateResult blocked(Plaza2ErrorCode error_code, std::string reason) {
    return {
        .allowed = false,
        .error_code = error_code,
        .reason = std::move(reason),
    };
}

} // namespace

Plaza2ManualOperatorGateResult
Plaza2ManualOperatorGate::validate_transport_connect(std::string_view endpoint_host,
                                                     const Plaza2RuntimeArmState& arm_state) {
    if (!arm_state.test_plaza2_armed) {
        return blocked(Plaza2ErrorCode::InvalidConfiguration,
                       "PLAZA II TEST bring-up requires --armed-test-plaza2");
    }
    if (is_loopback_host(endpoint_host)) {
        return success();
    }
    if (!arm_state.test_network_armed) {
        return blocked(Plaza2ErrorCode::InvalidConfiguration,
                       "external PLAZA II TEST endpoint requires --armed-test-network");
    }
    return success();
}

Plaza2ManualOperatorGateResult
Plaza2ManualOperatorGate::validate_session_start(std::string_view endpoint_host,
                                                 const Plaza2RuntimeArmState& arm_state) {
    const auto transport_gate = validate_transport_connect(endpoint_host, arm_state);
    if (!transport_gate.allowed) {
        return transport_gate;
    }
    if (is_loopback_host(endpoint_host)) {
        return success();
    }
    if (!arm_state.test_session_armed) {
        return blocked(Plaza2ErrorCode::InvalidConfiguration,
                       "external PLAZA II TEST session requires --armed-test-session");
    }
    return success();
}

} // namespace moex::plaza2::cgate
