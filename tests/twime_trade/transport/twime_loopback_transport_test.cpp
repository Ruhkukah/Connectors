#include "moex/twime_trade/transport/twime_loopback_transport.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::transport::TwimeLoopbackTransport transport;
        auto result = transport.open();
        moex::twime_sbe::test::require(result.status == moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                       "loopback transport failed to open");

        const auto establish_ack =
            moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("EstablishmentAck"));
        const auto sequence = moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("Sequence"));
        const auto written =
            moex::twime_trade::test::concatenate_frames(std::vector<std::vector<std::byte>>{establish_ack, sequence});

        result = transport.write(written);
        moex::twime_sbe::test::require(result.status == moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                       "loopback transport failed to write");
        moex::twime_sbe::test::require(result.bytes_transferred == written.size(),
                                       "loopback transport did not accept all bytes");

        std::vector<std::byte> read_buffer(written.size());
        const auto poll = transport.poll_read(read_buffer);
        moex::twime_sbe::test::require(poll.status == moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                       "loopback transport failed to read");
        moex::twime_sbe::test::require(poll.bytes_transferred == written.size(),
                                       "loopback transport read size mismatch");
        moex::twime_sbe::test::require(read_buffer == written, "loopback transport did not echo bytes exactly");

        const auto decoded_messages = moex::twime_trade::test::decode_streamed_frames(read_buffer);
        moex::twime_sbe::test::require(decoded_messages.size() == 2, "loopback transport did not preserve both frames");
        moex::twime_sbe::test::require(decoded_messages[0].metadata->name == "EstablishmentAck",
                                       "first decoded frame mismatch");
        moex::twime_sbe::test::require(decoded_messages[1].metadata->name == "Sequence",
                                       "second decoded frame mismatch");

        const auto metrics = transport.metrics();
        moex::twime_sbe::test::require(metrics.write_calls == 1, "loopback transport write_calls mismatch");
        moex::twime_sbe::test::require(metrics.read_calls == 1, "loopback transport read_calls mismatch");
        moex::twime_sbe::test::require(metrics.bytes_written == written.size(),
                                       "loopback transport bytes_written mismatch");
        moex::twime_sbe::test::require(metrics.bytes_read == written.size(), "loopback transport bytes_read mismatch");
        moex::twime_sbe::test::require(metrics.max_read_buffer_depth == written.size(),
                                       "loopback transport read-buffer high watermark mismatch");
        moex::twime_sbe::test::require(metrics.max_write_buffer_depth == written.size(),
                                       "loopback transport write-buffer high watermark mismatch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
