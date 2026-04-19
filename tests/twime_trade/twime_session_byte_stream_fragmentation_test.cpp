#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_trade/transport/twime_scripted_transport.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "byte_stream_fragmentation_test";
        moex::twime_trade::transport::TwimeScriptedTransport transport;
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        static_cast<void>(transport.drain_written_bytes());

        auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
        for (auto& field : ack.fields) {
            if (field.name == "NextSeqNo") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(11);
            }
        }
        transport.queue_read_bytes(moex::twime_trade::test::encode_bytes(ack), 1);
        session.poll_transport();

        moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Active,
                                               "fragmented byte-stream EstablishmentAck did not assemble");
        moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 11,
                                       "fragmented EstablishmentAck did not set expected inbound sequence");
        moex::twime_sbe::test::require(transport.metrics().partial_read_events > 0,
                                       "fragmented byte-stream path did not record partial reads");

        {
            moex::twime_trade::transport::TwimeScriptedTransport bounded_transport;
            bounded_transport.set_max_buffered_bytes(4);
            bool threw = false;
            try {
                bounded_transport.queue_read_bytes(moex::twime_trade::test::encode_bytes(ack));
            } catch (const std::runtime_error&) {
                threw = true;
            }
            moex::twime_sbe::test::require(threw,
                                           "scripted transport must reject queued reads that exceed the buffer limit");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
