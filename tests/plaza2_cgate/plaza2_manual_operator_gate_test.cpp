#include "moex/plaza2/cgate/plaza2_manual_operator_gate.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <iostream>

int main() {
    try {
        using namespace moex::plaza2::cgate;
        using namespace moex::plaza2::test;

        {
            Plaza2RuntimeArmState arm_state;
            const auto connect_gate = Plaza2ManualOperatorGate::validate_transport_connect("127.0.0.1", arm_state);
            require(!connect_gate.allowed, "loopback bring-up must still require --armed-test-plaza2");
        }

        {
            Plaza2RuntimeArmState arm_state;
            arm_state.test_plaza2_armed = true;
            const auto connect_gate = Plaza2ManualOperatorGate::validate_transport_connect("127.0.0.1", arm_state);
            const auto session_gate = Plaza2ManualOperatorGate::validate_session_start("127.0.0.1", arm_state);
            require(connect_gate.allowed, "loopback connect should be allowed when --armed-test-plaza2 is set");
            require(session_gate.allowed, "loopback session should be allowed when --armed-test-plaza2 is set");
        }

        {
            Plaza2RuntimeArmState arm_state;
            arm_state.test_plaza2_armed = true;
            const auto connect_gate = Plaza2ManualOperatorGate::validate_transport_connect("198.51.100.10", arm_state);
            require(!connect_gate.allowed, "external TEST connect must require --armed-test-network");
        }

        {
            Plaza2RuntimeArmState arm_state;
            arm_state.test_plaza2_armed = true;
            arm_state.test_network_armed = true;
            const auto session_gate = Plaza2ManualOperatorGate::validate_session_start("198.51.100.10", arm_state);
            require(!session_gate.allowed, "external TEST session must require --armed-test-session");
        }

        {
            Plaza2RuntimeArmState arm_state;
            arm_state.test_plaza2_armed = true;
            arm_state.test_network_armed = true;
            arm_state.test_session_armed = true;
            const auto session_gate = Plaza2ManualOperatorGate::validate_session_start("198.51.100.10", arm_state);
            require(session_gate.allowed, "external TEST session should be allowed with all arm flags present");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
