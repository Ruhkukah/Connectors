#include "moex/twime_trade/transport/twime_test_network_gate.hpp"

#include "moex/twime_trade/transport/twime_endpoint_resolver.hpp"

#include <cstdlib>

namespace moex::twime_trade::transport {

namespace {

bool env_flag_enabled(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return false;
    }

    const std::string_view text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "YES" || text == "on" ||
           text == "ON";
}

TwimeEndpointValidationResult block_result(TwimeTransportErrorCode error_code, std::string summary) {
    return {
        .allowed = false,
        .loopback = false,
        .error_code = error_code,
        .event = TwimeTransportEvent::OpenFailed,
        .summary = std::move(summary),
    };
}

} // namespace

TwimeEndpointValidationResult validate_twime_endpoint(const TwimeTcpConfig& config,
                                                      const TwimeRuntimeArmState& arm_state) {
    if (config.environment != TwimeTcpEnvironment::Test) {
        return block_result(TwimeTransportErrorCode::EnvironmentBlocked,
                            "TWIME TCP transport remains test-only in Phase 2E");
    }
    if (config.endpoint.port == 0) {
        return block_result(TwimeTransportErrorCode::InvalidConfiguration,
                            "TWIME TCP transport requires a non-zero port");
    }
    if (config.endpoint.host.empty() || twime_is_placeholder_host(config.endpoint.host)) {
        return block_result(TwimeTransportErrorCode::InvalidConfiguration,
                            "TWIME TCP transport requires a concrete endpoint host at runtime");
    }

    if (twime_is_explicit_loopback_host(config.endpoint.host)) {
        return {
            .allowed = true,
            .loopback = true,
            .error_code = TwimeTransportErrorCode::None,
            .event = TwimeTransportEvent::OpenStarted,
            .summary = "loopback TWIME TCP endpoint allowed",
        };
    }

    if (!config.test_network_gate.external_test_endpoint_enabled) {
        return block_result(TwimeTransportErrorCode::LocalOnlyViolation,
                            "Phase 2E requires external_test_endpoint_enabled=true for non-loopback test endpoints");
    }
    if (config.test_network_gate.block_production_like_hostnames &&
        twime_host_looks_production_like(config.endpoint.host)) {
        return block_result(TwimeTransportErrorCode::LocalOnlyViolation,
                            "Phase 2E blocks production-like MOEX or broker hostnames in test mode");
    }
    if (config.test_network_gate.block_private_nonlocal_networks_by_default &&
        twime_host_is_private_nonlocal_ipv4(config.endpoint.host)) {
        return block_result(TwimeTransportErrorCode::LocalOnlyViolation,
                            "Phase 2E blocks private non-loopback IPv4 targets unless a later phase enables them");
    }
    if (config.test_network_gate.require_explicit_runtime_arm && !arm_state.test_network_armed) {
        return block_result(
            TwimeTransportErrorCode::LocalOnlyViolation,
            "Phase 2E requires --armed-test-network or MOEX_ARM_TEST_NETWORK=1 for non-loopback test endpoints");
    }

    return {
        .allowed = true,
        .loopback = false,
        .error_code = TwimeTransportErrorCode::None,
        .event = TwimeTransportEvent::OpenStarted,
        .summary = "non-loopback TWIME test endpoint allowed under explicit test-network arm",
    };
}

TwimeTestNetworkGate::TwimeTestNetworkGate(TwimeRuntimeArmState arm_state, TwimeTcpConfig config)
    : arm_state_(arm_state), config_(std::move(config)) {}

TwimeEndpointValidationResult TwimeTestNetworkGate::validate_before_open() const {
    return validate_twime_endpoint(config_, effective_arm_state());
}

TwimeRuntimeArmState TwimeTestNetworkGate::effective_arm_state() const {
    auto effective = arm_state_;
    effective.prod_armed = effective.prod_armed || config_.runtime_arm_state.prod_armed;
    effective.test_network_armed = effective.test_network_armed || config_.runtime_arm_state.test_network_armed;
    effective.test_network_armed = effective.test_network_armed || env_flag_enabled("MOEX_ARM_TEST_NETWORK");
    return effective;
}

} // namespace moex::twime_trade::transport
