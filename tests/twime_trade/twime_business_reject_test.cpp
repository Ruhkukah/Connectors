#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "business_reject_test";
        moex::twime_trade::TwimeFakeTransport transport;
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
        for (auto& field : ack.fields) {
            if (field.name == "NextSeqNo") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(11);
            }
        }
        moex::twime_trade::test::script_message(transport, ack);
        session.poll_transport();
        static_cast<void>(session.drain_events());

        auto reject = moex::twime_trade::test::make_request("BusinessMessageReject");
        for (auto& field : reject.fields) {
            if (field.name == "OrdRejReason") {
                field.value = moex::twime_sbe::TwimeFieldValue::signed_integer(-12);
            }
        }
        moex::twime_trade::test::script_message(transport, reject);
        session.poll_transport();

        auto events = session.drain_events();
        const auto* event = moex::twime_trade::test::find_last_event(
            events, moex::twime_trade::TwimeSessionEventType::BusinessRejectReceived);
        moex::twime_sbe::test::require(event != nullptr, "BusinessMessageReject event missing");
        moex::twime_sbe::test::require(event->reason_code == -12, "BusinessMessageReject reason code mismatch");
        const auto inbound_entry = session.inbound_journal().last_n(1).front();
        moex::twime_sbe::test::require(inbound_entry.message_name == "BusinessMessageReject",
                                       "BusinessMessageReject journal entry missing");
        const auto decoded = moex::twime_trade::test::decode_bytes(inbound_entry.bytes);
        const auto* ord_rej_reason = moex::twime_trade::test::find_field(decoded, "OrdRejReason");
        moex::twime_sbe::test::require(ord_rej_reason != nullptr,
                                       "OrdRejReason field missing from active schema decode");
        moex::twime_sbe::test::require(ord_rej_reason->value.signed_value == -12,
                                       "OrdRejReason value mismatch in active schema decode");
        moex::twime_sbe::test::require(inbound_entry.sequence_number == 0,
                                       "BusinessMessageReject must not consume inbound application sequence");
        moex::twime_sbe::test::require(!inbound_entry.recoverable,
                                       "BusinessMessageReject must not be marked recoverable");
        moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 11,
                                       "BusinessMessageReject must not advance expected inbound sequence");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
