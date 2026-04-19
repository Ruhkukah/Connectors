#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_tcp_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::test::LocalTcpServer server;
        moex::twime_trade::transport::TwimeTcpTransport transport(
            moex::twime_trade::test::make_local_tcp_config(server.port()));

        const auto open_result = transport.open();
        moex::twime_sbe::test::require(
            open_result.event == moex::twime_trade::transport::TwimeTransportEvent::OpenStarted ||
                open_result.event == moex::twime_trade::transport::TwimeTransportEvent::OpenSucceeded,
            "TCP transport must report open start or success");

        moex::twime_trade::test::require_tcp_open(transport);
        moex::twime_sbe::test::require(server.wait_for_client(std::chrono::milliseconds(3000)),
                                       "loopback TCP server did not observe client connection");
        moex::twime_sbe::test::require(transport.state() == moex::twime_trade::transport::TwimeTransportState::Open,
                                       "TCP transport did not reach Open state");
        const auto metrics = transport.metrics();
        moex::twime_sbe::test::require(metrics.open_calls == 1, "TCP transport open_calls mismatch");
        moex::twime_sbe::test::require(metrics.successful_open_events == 1,
                                       "TCP transport successful open metric mismatch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
