#include "moex/twime_trade/transport/twime_test_network_gate.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        auto config = moex::twime_trade::test::make_external_test_tcp_config("198.51.100.10", 19051);
        config.runtime_arm_state.test_network_armed = true;

        const auto validation = moex::twime_trade::transport::validate_twime_endpoint(config, config.runtime_arm_state);
        moex::twime_sbe::test::require(validation.allowed && !validation.loopback,
                                       "armed external test endpoint should validate successfully");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
