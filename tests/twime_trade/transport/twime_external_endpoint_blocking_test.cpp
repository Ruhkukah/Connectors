#include "moex/twime_trade/transport/twime_test_network_gate.hpp"
#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        auto config = moex::twime_trade::test::make_external_test_tcp_config("198.51.100.10", 19050);
        const auto validation = moex::twime_trade::transport::validate_twime_endpoint(config, config.runtime_arm_state);
        moex::twime_sbe::test::require(
            !validation.allowed &&
                validation.error_code == moex::twime_trade::transport::TwimeTransportErrorCode::LocalOnlyViolation,
            "unarmed external endpoint should be blocked before socket open");

        moex::twime_trade::transport::TwimeTcpTransport transport(config);
        const auto open_result = transport.open();
        moex::twime_sbe::test::require(
            open_result.status == moex::twime_trade::transport::TwimeTransportStatus::Fault &&
                open_result.error_code == moex::twime_trade::transport::TwimeTransportErrorCode::LocalOnlyViolation,
            "transport open should fail before any external socket connect");
        const auto metrics = transport.metrics();
        moex::twime_sbe::test::require(metrics.open_calls == 1 && metrics.open_failed_events == 1 &&
                                           metrics.successful_open_events == 0,
                                       "blocked external open metrics mismatch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
