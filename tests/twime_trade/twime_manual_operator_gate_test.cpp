#include "moex/twime_trade/twime_manual_operator_gate.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        using namespace moex::twime_trade;
        using namespace moex::twime_trade::transport;

        {
            auto config = moex::twime_trade::test::make_local_tcp_config(19020);
            const auto connect_gate = TwimeManualOperatorGate::validate_transport_connect(config, {});
            const auto session_gate = TwimeManualOperatorGate::validate_session_start(config, {});
            moex::twime_sbe::test::require(connect_gate.allowed, "localhost transport connect must remain allowed");
            moex::twime_sbe::test::require(session_gate.allowed, "localhost session start must remain allowed");
        }

        {
            auto config = moex::twime_trade::test::make_external_test_tcp_config("198.51.100.10", 19021);
            const auto connect_gate = TwimeManualOperatorGate::validate_transport_connect(config, {});
            moex::twime_sbe::test::require(!connect_gate.allowed,
                                           "external endpoint must be blocked without --armed-test-network");
        }

        {
            auto config = moex::twime_trade::test::make_external_test_tcp_config("198.51.100.10", 19022);
            TwimeRuntimeArmState arm_state;
            arm_state.test_network_armed = true;
            const auto session_gate = TwimeManualOperatorGate::validate_session_start(config, arm_state);
            moex::twime_sbe::test::require(!session_gate.allowed,
                                           "external session start must be blocked without --armed-test-session");
        }

        {
            auto config = moex::twime_trade::test::make_external_test_tcp_config("198.51.100.10", 19023);
            TwimeRuntimeArmState arm_state;
            arm_state.test_network_armed = true;
            arm_state.test_session_armed = true;
            const auto session_gate = TwimeManualOperatorGate::validate_session_start(config, arm_state);
            moex::twime_sbe::test::require(session_gate.allowed,
                                           "external session start must be allowed when both arm flags are present");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
