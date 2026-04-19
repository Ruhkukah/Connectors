#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_tcp_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::test::LocalTcpServer server;
        auto config = moex::twime_trade::test::make_local_tcp_config(server.port());
        config.buffer_policy.write_chunk_bytes = 4;
        moex::twime_trade::transport::TwimeTcpTransport transport(config);

        static_cast<void>(transport.open());
        moex::twime_trade::test::require_tcp_open(transport);

        const auto payload = moex::twime_trade::test::concatenate_frames(
            {moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("Establish")),
             moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("Sequence"))});

        std::size_t offset = 0;
        while (offset < payload.size()) {
            const auto result = transport.write(std::span<const std::byte>(payload).subspan(offset));
            moex::twime_sbe::test::require(result.status == moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "TCP partial write returned non-OK status");
            moex::twime_sbe::test::require(result.bytes_transferred > 0,
                                           "TCP partial write reported zero bytes transferred");
            offset += result.bytes_transferred;
        }

        const auto received = server.receive_exact(payload.size(), std::chrono::milliseconds(3000));
        moex::twime_sbe::test::require(received == payload, "server did not receive exact TCP payload bytes");
        const auto metrics = transport.metrics();
        moex::twime_sbe::test::require(metrics.partial_write_events > 0, "TCP transport did not record partial writes");
        moex::twime_sbe::test::require(metrics.bytes_written == payload.size(), "TCP transport bytes_written mismatch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
