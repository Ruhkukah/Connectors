#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_trade/transport/twime_scripted_transport.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "byte_stream_establish_test";
            moex::twime_trade::transport::TwimeScriptedTransport transport;
            transport.set_max_write_size(5);
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Establishing,
                                                   "byte-stream session did not enter Establishing");

            const auto written = transport.drain_written_bytes();
            const auto outbound_messages = moex::twime_trade::test::decode_streamed_frames(written);
            moex::twime_sbe::test::require(outbound_messages.size() == 1, "ConnectFake must emit one Establish frame");
            moex::twime_sbe::test::require(outbound_messages.front().metadata->name == "Establish",
                                           "first outbound byte-stream frame must be Establish");

            auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
            for (auto& field : ack.fields) {
                if (field.name == "KeepaliveInterval") {
                    field.value = moex::twime_sbe::TwimeFieldValue::delta_millisecs(1500);
                }
                if (field.name == "NextSeqNo") {
                    field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(11);
                }
            }
            transport.queue_read_bytes(moex::twime_trade::test::encode_bytes(ack));
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Active,
                                                   "byte-stream session did not become Active");
            moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 11,
                                           "byte-stream EstablishmentAck did not initialize inbound sequence");
            const auto metrics = transport.metrics();
            moex::twime_sbe::test::require(metrics.partial_write_events > 0,
                                           "scripted transport partial-write path was not exercised");
            moex::twime_sbe::test::require(metrics.write_calls > 1,
                                           "scripted transport partial-write path did not require multiple writes");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "byte_stream_remote_close_test";
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
            transport.queue_read_bytes(moex::twime_trade::test::encode_bytes(ack));
            session.poll_transport();
            static_cast<void>(session.drain_events());

            transport.queue_remote_close();
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Faulted,
                                                   "byte-stream remote close must fault active session");
            const auto events = session.drain_events();
            moex::twime_sbe::test::require(moex::twime_trade::test::find_last_event(
                                               events, moex::twime_trade::TwimeSessionEventType::PeerClosed) != nullptr,
                                           "byte-stream remote close did not emit PeerClosed");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "byte_stream_fault_test";
            moex::twime_trade::transport::TwimeScriptedTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
            static_cast<void>(transport.drain_written_bytes());
            transport.queue_read_fault();
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Faulted,
                                                   "byte-stream read fault must fault session");
            moex::twime_sbe::test::require(transport.metrics().fault_events == 1,
                                           "scripted transport read fault metrics mismatch");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
