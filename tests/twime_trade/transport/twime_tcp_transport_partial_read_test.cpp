#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_tcp_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::test::LocalTcpServer server;
        auto config = moex::twime_trade::test::make_local_tcp_config(server.port());
        config.buffer_policy.read_chunk_bytes = 3;
        moex::twime_trade::transport::TwimeTcpTransport transport(config);

        static_cast<void>(transport.open());
        moex::twime_trade::test::require_tcp_open(transport);

        const auto ack =
            moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("EstablishmentAck"));
        server.send_bytes(ack, 1);

        std::vector<std::byte> accumulated;
        std::vector<std::byte> read_buffer(16);
        while (accumulated.size() < ack.size()) {
            const auto result = transport.poll_read(read_buffer);
            if (result.status == moex::twime_trade::transport::TwimeTransportStatus::WouldBlock) {
                continue;
            }
            moex::twime_sbe::test::require(result.status == moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "TCP partial read returned non-OK status");
            accumulated.insert(accumulated.end(), read_buffer.begin(),
                               read_buffer.begin() + static_cast<std::ptrdiff_t>(result.bytes_transferred));
        }

        moex::twime_sbe::test::require(accumulated == ack, "TCP transport partial read bytes mismatch");
        const auto metrics = transport.metrics();
        moex::twime_sbe::test::require(metrics.partial_read_events > 0, "TCP transport did not record partial reads");
        moex::twime_sbe::test::require(metrics.bytes_read == ack.size(), "TCP transport bytes_read mismatch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
