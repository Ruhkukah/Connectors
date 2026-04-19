#include "moex/twime_trade/transport/twime_loopback_transport.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        {
            moex::twime_trade::transport::TwimeLoopbackTransport transport;
            moex::twime_sbe::test::require(transport.open().status ==
                                               moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "loopback transport failed to open for write-fault case");
            transport.inject_next_write_fault();

            const auto bytes =
                moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("Establish"));
            const auto result = transport.write(bytes);
            moex::twime_sbe::test::require(result.status == moex::twime_trade::transport::TwimeTransportStatus::Fault,
                                           "write-fault injection did not return Fault");
            moex::twime_sbe::test::require(transport.state() ==
                                               moex::twime_trade::transport::TwimeTransportState::Faulted,
                                           "write-fault injection did not fault the transport");
            moex::twime_sbe::test::require(transport.metrics().fault_events == 1,
                                           "write-fault injection fault_events mismatch");
        }

        {
            moex::twime_trade::transport::TwimeLoopbackTransport transport;
            moex::twime_sbe::test::require(transport.open().status ==
                                               moex::twime_trade::transport::TwimeTransportStatus::Ok,
                                           "loopback transport failed to open for read-fault case");
            transport.inject_next_read_fault();

            std::vector<std::byte> buffer(32);
            const auto result = transport.poll_read(buffer);
            moex::twime_sbe::test::require(result.status == moex::twime_trade::transport::TwimeTransportStatus::Fault,
                                           "read-fault injection did not return Fault");
            moex::twime_sbe::test::require(transport.state() ==
                                               moex::twime_trade::transport::TwimeTransportState::Faulted,
                                           "read-fault injection did not fault the transport");
            moex::twime_sbe::test::require(transport.metrics().fault_events == 1,
                                           "read-fault injection fault_events mismatch");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
