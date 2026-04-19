#include "moex/twime_trade/transport/twime_test_network_gate.hpp"

#include "twime_trade_test_support.hpp"

#include <cstdlib>
#include <iostream>

namespace {

struct ScopedEnvVar {
    explicit ScopedEnvVar(const char* value) {
        if (value == nullptr) {
            ::unsetenv("MOEX_ARM_TEST_NETWORK");
        } else {
            ::setenv("MOEX_ARM_TEST_NETWORK", value, 1);
        }
    }

    ~ScopedEnvVar() {
        ::unsetenv("MOEX_ARM_TEST_NETWORK");
    }
};

} // namespace

int main() {
    try {
        using namespace moex::twime_trade::transport;

        {
            const ScopedEnvVar clear_env(nullptr);
            auto config = moex::twime_trade::test::make_local_tcp_config(19010);
            const auto result = validate_twime_endpoint(config, {});
            moex::twime_sbe::test::require(result.allowed && result.loopback,
                                           "localhost loopback transport should be allowed without arming");
        }

        {
            const ScopedEnvVar clear_env(nullptr);
            auto config = moex::twime_trade::test::make_external_test_tcp_config("198.51.100.10", 19011);
            const auto result = validate_twime_endpoint(config, {});
            moex::twime_sbe::test::require(!result.allowed &&
                                               result.error_code == TwimeTransportErrorCode::LocalOnlyViolation,
                                           "external test endpoint must be blocked without explicit arming");
        }

        {
            const ScopedEnvVar clear_env(nullptr);
            auto config = moex::twime_trade::test::make_external_test_tcp_config("198.51.100.10", 19012);
            TwimeRuntimeArmState arm_state;
            arm_state.test_network_armed = true;
            const auto result = validate_twime_endpoint(config, arm_state);
            moex::twime_sbe::test::require(result.allowed && !result.loopback,
                                           "armed external test endpoint should be allowed");
        }

        {
            const ScopedEnvVar armed_env("1");
            auto config = moex::twime_trade::test::make_external_test_tcp_config("198.51.100.10", 19013);
            const auto result = TwimeTestNetworkGate({}, config).validate_before_open();
            moex::twime_sbe::test::require(result.allowed,
                                           "MOEX_ARM_TEST_NETWORK=1 should satisfy the explicit runtime arm");
        }

        {
            const ScopedEnvVar clear_env(nullptr);
            auto config = moex::twime_trade::test::make_external_test_tcp_config("moex-gateway.example", 19014);
            TwimeRuntimeArmState arm_state;
            arm_state.test_network_armed = true;
            const auto result = validate_twime_endpoint(config, arm_state);
            moex::twime_sbe::test::require(!result.allowed &&
                                               result.error_code == TwimeTransportErrorCode::LocalOnlyViolation,
                                           "production-like hostnames must remain blocked in test mode");
        }

        {
            const ScopedEnvVar clear_env(nullptr);
            auto config = moex::twime_trade::test::make_external_test_tcp_config("198.51.100.10", 19015);
            config.environment = TwimeTcpEnvironment::Prod;
            TwimeRuntimeArmState arm_state;
            arm_state.test_network_armed = true;
            const auto result = validate_twime_endpoint(config, arm_state);
            moex::twime_sbe::test::require(!result.allowed &&
                                               result.error_code == TwimeTransportErrorCode::EnvironmentBlocked,
                                           "prod environment must remain blocked in Phase 2E");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
