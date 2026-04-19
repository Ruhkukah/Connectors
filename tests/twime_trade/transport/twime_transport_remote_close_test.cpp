#include "moex/twime_trade/transport/twime_loopback_transport.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::transport::TwimeLoopbackTransport transport;
        moex::twime_sbe::test::require(transport.open().status ==
                                           moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                       "loopback transport failed to open");

        const auto ack =
            moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("EstablishmentAck"));
        transport.queue_inbound_bytes(ack);
        transport.script_remote_close();

        std::vector<std::byte> buffer(ack.size());
        const auto first_read = transport.poll_read(buffer);
        moex::twime_sbe::test::require(first_read.status == moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                       "remote-close transport failed to return queued bytes");
        moex::twime_sbe::test::require(std::vector<std::byte>(buffer.begin(), buffer.end()) == ack,
                                       "remote-close transport returned wrong queued bytes");

        const auto second_read = transport.poll_read(buffer);
        moex::twime_sbe::test::require(second_read.status ==
                                           moex::twime_trade::transport::TwimeTransportStatus::RemoteClosed,
                                       "remote-close transport did not report remote close");
        moex::twime_sbe::test::require(transport.state() == moex::twime_trade::transport::TwimeTransportState::Closed,
                                       "remote-close transport did not enter Closed state");
        moex::twime_sbe::test::require(transport.metrics().remote_close_events == 1,
                                       "remote-close transport remote_close_events mismatch");

        {
            moex::twime_trade::transport::TwimeLoopbackTransport local_close_transport;
            moex::twime_sbe::test::require(local_close_transport.open().status ==
                                               moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "local-close transport failed to open");
            const auto close_result = local_close_transport.close();
            moex::twime_sbe::test::require(close_result.status ==
                                               moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "local close did not return success");
            moex::twime_sbe::test::require(local_close_transport.state() ==
                                               moex::twime_trade::transport::TwimeTransportState::Closed,
                                           "local close did not enter Closed state");
            const auto write_after_close = local_close_transport.write(ack);
            moex::twime_sbe::test::require(write_after_close.status ==
                                               moex::twime_trade::transport::TwimeTransportStatus::InvalidState,
                                           "local close must reject writes after close");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
