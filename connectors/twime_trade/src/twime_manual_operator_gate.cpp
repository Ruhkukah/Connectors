#include "moex/twime_trade/twime_manual_operator_gate.hpp"

#include "moex/twime_trade/transport/twime_endpoint_resolver.hpp"

namespace moex::twime_trade {

namespace {

TwimeManualOperatorGateResult success() {
    return {};
}

TwimeManualOperatorGateResult blocked(transport::TwimeTransportErrorCode error_code, std::string reason) {
    return {
        .allowed = false,
        .error_code = error_code,
        .reason = std::move(reason),
    };
}

} // namespace

TwimeManualOperatorGateResult
TwimeManualOperatorGate::validate_transport_connect(const transport::TwimeTcpConfig& config,
                                                    const transport::TwimeRuntimeArmState& arm_state) {
    if (transport::twime_is_explicit_loopback_host(config.endpoint.host)) {
        return {.allowed = true};
    }
    if (config.environment != transport::TwimeTcpEnvironment::Test) {
        return blocked(transport::TwimeTransportErrorCode::EnvironmentBlocked,
                       "Phase 2F external session path is blocked outside environment=test");
    }
    if (!arm_state.test_network_armed) {
        return blocked(transport::TwimeTransportErrorCode::LocalOnlyViolation,
                       "external TWIME test endpoint requires --armed-test-network");
    }
    return {.allowed = true};
}

TwimeManualOperatorGateResult
TwimeManualOperatorGate::validate_session_start(const transport::TwimeTcpConfig& config,
                                                const transport::TwimeRuntimeArmState& arm_state) {
    const auto connect_gate = validate_transport_connect(config, arm_state);
    if (!connect_gate.allowed) {
        return connect_gate;
    }
    if (transport::twime_is_explicit_loopback_host(config.endpoint.host)) {
        return {.allowed = true};
    }
    if (!arm_state.test_session_armed) {
        return blocked(transport::TwimeTransportErrorCode::EnvironmentBlocked,
                       "external TWIME test session requires --armed-test-session");
    }
    return {.allowed = true};
}

} // namespace moex::twime_trade
