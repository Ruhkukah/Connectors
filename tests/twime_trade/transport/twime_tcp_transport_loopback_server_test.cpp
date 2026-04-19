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

        const auto outbound = moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("Establish"));
        std::size_t offset = 0;
        while (offset < outbound.size()) {
            const auto result = transport.write(std::span<const std::byte>(outbound).subspan(offset));
            moex::twime_sbe::test::require(result.status == moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "TCP transport failed to write to loopback server");
            offset += result.bytes_transferred;
        }

        const auto received = server.receive_exact(outbound.size(), std::chrono::milliseconds(3000));
        moex::twime_sbe::test::require(received == outbound, "loopback TCP server received mismatched bytes");

        const auto inbound =
            moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("EstablishmentAck"));
        server.send_bytes(inbound);

        std::vector<std::byte> accumulated;
        std::vector<std::byte> read_buffer(256);
        while (accumulated.size() < inbound.size()) {
            const auto poll = transport.poll_read(read_buffer);
            if (poll.status == moex::twime_trade::transport::TwimeTransportStatus::WouldBlock) {
                continue;
            }
            moex::twime_sbe::test::require(poll.status == moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "TCP transport failed to read from loopback server");
            accumulated.insert(accumulated.end(), read_buffer.begin(),
                               read_buffer.begin() + static_cast<std::ptrdiff_t>(poll.bytes_transferred));
        }
        moex::twime_sbe::test::require(accumulated == inbound, "loopback TCP server inbound bytes mismatch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
