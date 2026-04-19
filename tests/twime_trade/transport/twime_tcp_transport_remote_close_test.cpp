#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_tcp_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::test::LocalTcpServer server;
        moex::twime_trade::transport::TwimeTcpTransport transport(
            moex::twime_trade::test::make_local_tcp_config(server.port()));

        static_cast<void>(transport.open());
        moex::twime_trade::test::require_tcp_open(transport);
        server.send_bytes(moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("Sequence")));
        server.close_client();

        bool saw_payload = false;
        for (int attempt = 0; attempt < 64 && !saw_payload; ++attempt) {
            std::vector<std::byte> buffer(256);
            const auto poll = transport.poll_read(buffer);
            if (poll.status == moex::twime_trade::transport::TwimeTransportStatus::Ok && poll.bytes_transferred > 0) {
                saw_payload = true;
            }
        }
        moex::twime_sbe::test::require(saw_payload,
                                       "TCP transport remote-close test did not receive the final payload chunk");

        const auto result = moex::twime_trade::test::poll_transport_until(
            transport,
            [](const auto& candidate, const auto& candidate_transport) {
                return candidate.status == moex::twime_trade::transport::TwimeTransportStatus::RemoteClosed ||
                       (candidate_transport.state() == moex::twime_trade::transport::TwimeTransportState::Closed &&
                        candidate_transport.metrics().remote_close_events > 0);
            },
            512);

        moex::twime_sbe::test::require(
            result.status == moex::twime_trade::transport::TwimeTransportStatus::RemoteClosed ||
                (transport.state() == moex::twime_trade::transport::TwimeTransportState::Closed &&
                 transport.metrics().remote_close_events > 0),
            "TCP transport did not surface remote close");
        moex::twime_sbe::test::require(transport.state() == moex::twime_trade::transport::TwimeTransportState::Closed,
                                       "TCP transport did not enter Closed after remote close");
        moex::twime_sbe::test::require(transport.metrics().remote_close_events == 1,
                                       "TCP transport remote_close metric mismatch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
