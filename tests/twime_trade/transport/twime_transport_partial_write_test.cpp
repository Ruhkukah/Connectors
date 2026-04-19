#include "moex/twime_trade/transport/twime_loopback_transport.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::transport::TwimeLoopbackTransport transport;
        transport.set_max_write_size(5);
        moex::twime_sbe::test::require(transport.open().status ==
                                           moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                       "loopback transport failed to open");

        const auto establish =
            moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("Establish"));
        std::size_t offset = 0;
        bool observed_partial_write = false;
        while (offset < establish.size()) {
            const auto write_result = transport.write(std::span<const std::byte>(establish).subspan(offset));
            moex::twime_sbe::test::require(write_result.status ==
                                               moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "partial-write transport returned unexpected status");
            observed_partial_write =
                observed_partial_write ||
                write_result.event == moex::twime_trade::transport::TwimeTransportEvent::PartialWrite;
            offset += write_result.bytes_transferred;
        }

        moex::twime_sbe::test::require(observed_partial_write,
                                       "partial-write transport never reported a partial write");
        const auto written = transport.drain_written_bytes();
        moex::twime_sbe::test::require(written == establish, "partial-write transport lost outbound bytes");

        std::vector<std::byte> chunk(establish.size());
        std::vector<std::byte> received;
        while (true) {
            const auto poll = transport.poll_read(chunk);
            if (poll.status == moex::twime_trade::transport::TwimeTransportStatus::WouldBlock) {
                break;
            }
            moex::twime_sbe::test::require(poll.status == moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "partial-write transport returned unexpected read status");
            received.insert(received.end(), chunk.begin(),
                            chunk.begin() + static_cast<std::ptrdiff_t>(poll.bytes_transferred));
        }

        moex::twime_sbe::test::require(received == establish, "partial-write loopback read bytes mismatch");

        {
            moex::twime_trade::transport::TwimeLoopbackTransport bounded_transport;
            bounded_transport.set_max_buffered_bytes(4);
            moex::twime_sbe::test::require(bounded_transport.open().status ==
                                               moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "bounded loopback transport failed to open");

            const auto first_write = bounded_transport.write(std::span<const std::byte>(establish).first(4));
            moex::twime_sbe::test::require(first_write.status == moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "bounded loopback transport first write failed");
            const auto second_write = bounded_transport.write(std::span<const std::byte>(establish).subspan(4));
            moex::twime_sbe::test::require(second_write.status ==
                                               moex::twime_trade::transport::TwimeTransportStatus::WouldBlock,
                                           "bounded loopback transport must backpressure instead of growing unbounded");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
