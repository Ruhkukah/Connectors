#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_trade/transport/twime_scripted_transport.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "byte_stream_batched_frames_test";
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

        auto sequence = moex::twime_trade::test::make_request("Sequence");
        for (auto& field : sequence.fields) {
            if (field.name == "NextSeqNo") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(14);
            }
        }

        const auto batched = moex::twime_trade::test::concatenate_frames(std::vector<std::vector<std::byte>>{
            moex::twime_trade::test::encode_bytes(ack),
            moex::twime_trade::test::encode_bytes(sequence),
        });
        transport.queue_read_bytes(batched);
        session.poll_transport();

        moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Recovering,
                                               "batched inbound frames did not trigger Recovering state");

        const auto outbound = transport.drain_written_bytes();
        const auto outbound_messages = moex::twime_trade::test::decode_streamed_frames(outbound);
        moex::twime_sbe::test::require(outbound_messages.size() == 1,
                                       "batched byte-stream gap should emit one RetransmitRequest");
        moex::twime_sbe::test::require(outbound_messages.front().metadata->name == "RetransmitRequest",
                                       "batched byte-stream gap did not emit RetransmitRequest");

        const auto* from_seq_no = moex::twime_trade::test::find_field(outbound_messages.front(), "FromSeqNo");
        const auto* count = moex::twime_trade::test::find_field(outbound_messages.front(), "Count");
        moex::twime_sbe::test::require(from_seq_no != nullptr && from_seq_no->value.unsigned_value == 11,
                                       "batched byte-stream RetransmitRequest FromSeqNo mismatch");
        moex::twime_sbe::test::require(count != nullptr && count->value.unsigned_value == 3,
                                       "batched byte-stream RetransmitRequest Count mismatch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
