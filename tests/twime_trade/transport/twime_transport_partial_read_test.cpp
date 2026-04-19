#include "moex/twime_trade/transport/twime_loopback_transport.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::transport::TwimeLoopbackTransport transport;
        transport.set_max_read_size(3);
        moex::twime_sbe::test::require(transport.open().status ==
                                           moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                       "loopback transport failed to open");

        const auto ack =
            moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("EstablishmentAck"));
        const auto sequence = moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("Sequence"));
        const auto payload =
            moex::twime_trade::test::concatenate_frames(std::vector<std::vector<std::byte>>{ack, sequence});
        transport.queue_inbound_bytes(payload);

        std::vector<std::byte> chunk(64);
        std::vector<std::byte> received;
        while (true) {
            const auto poll = transport.poll_read(chunk);
            if (poll.status == moex::twime_trade::transport::TwimeTransportStatus::WouldBlock) {
                break;
            }
            moex::twime_sbe::test::require(poll.status == moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "partial-read transport returned unexpected status");
            received.insert(received.end(), chunk.begin(),
                            chunk.begin() + static_cast<std::ptrdiff_t>(poll.bytes_transferred));
        }

        moex::twime_sbe::test::require(received == payload, "partial-read transport bytes mismatch");
        const auto metrics = transport.metrics();
        moex::twime_sbe::test::require(metrics.partial_read_events > 0,
                                       "partial-read transport did not record partial reads");
        moex::twime_sbe::test::require(metrics.max_read_buffer_depth == payload.size(),
                                       "partial-read transport high watermark mismatch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
