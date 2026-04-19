#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_tcp_test_support.hpp"

#include <iostream>

namespace {

std::uint16_t reserve_unused_local_port() {
    moex::test::LocalTcpServer server;
    const auto port = server.port();
    server.stop();
    return port;
}

} // namespace

int main() {
    try {
        {
            auto config = moex::twime_trade::test::make_local_tcp_config(reserve_unused_local_port());
            moex::twime_trade::transport::TwimeTcpTransport transport(config);
            const auto open_result = transport.open();
            if (open_result.status == moex::twime_trade::transport::TwimeTransportStatus::WouldBlock) {
                const auto poll_result =
                    moex::twime_trade::test::poll_transport_until(transport, [](const auto& candidate, const auto&) {
                        return candidate.status == moex::twime_trade::transport::TwimeTransportStatus::Fault;
                    });
                moex::twime_sbe::test::require(
                    poll_result.status == moex::twime_trade::transport::TwimeTransportStatus::Fault,
                    "TCP transport did not fault after connection attempt to an unopened local port");
            } else {
                moex::twime_sbe::test::require(open_result.status ==
                                                   moex::twime_trade::transport::TwimeTransportStatus::Fault,
                                               "TCP transport unexpected status for unopened local port");
            }
        }

        {
            auto config = moex::twime_trade::test::make_local_tcp_config(12000);
            config.environment = moex::twime_trade::transport::TwimeTcpEnvironment::Prod;
            moex::twime_trade::transport::TwimeTcpTransport transport(config);
            const auto open_result = transport.open();
            moex::twime_sbe::test::require(open_result.status ==
                                               moex::twime_trade::transport::TwimeTransportStatus::Fault,
                                           "TCP transport must reject non-test environments");
            moex::twime_sbe::test::require(
                open_result.error_code == moex::twime_trade::transport::TwimeTransportErrorCode::EnvironmentBlocked,
                "TCP transport non-test environment error mismatch");
        }

        {
            auto config = moex::twime_trade::test::make_local_tcp_config(12001);
            config.endpoint.host = "198.51.100.10";
            moex::twime_trade::transport::TwimeTcpTransport transport(config);
            const auto open_result = transport.open();
            moex::twime_sbe::test::require(open_result.status ==
                                               moex::twime_trade::transport::TwimeTransportStatus::Fault,
                                           "TCP transport must reject non-loopback endpoints by default");
            moex::twime_sbe::test::require(
                open_result.error_code == moex::twime_trade::transport::TwimeTransportErrorCode::LocalOnlyViolation,
                "TCP transport non-loopback endpoint error mismatch");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
