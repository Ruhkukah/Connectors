#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "retransmission_test";
        moex::twime_trade::TwimeFakeTransport transport;
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
        moex::twime_trade::test::script_message(transport, ack);
        session.poll_transport();
        static_cast<void>(session.drain_events());

        auto sequence = moex::twime_trade::test::make_request("Sequence");
        for (auto& field : sequence.fields) {
            if (field.name == "NextSeqNo") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(6);
            }
        }
        moex::twime_trade::test::script_message(transport, sequence);
        session.poll_transport();
        static_cast<void>(session.drain_events());

        auto retransmission = moex::twime_trade::test::make_request("Retransmission");
        for (auto& field : retransmission.fields) {
            if (field.name == "NextSeqNo") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(6);
            }
            if (field.name == "Count") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(5);
            }
        }
        moex::twime_trade::test::script_message(transport, retransmission);
        session.poll_transport();

        moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Active,
                                               "Retransmission did not restore Active state");
        auto events = session.drain_events();
        moex::twime_sbe::test::require(
            moex::twime_trade::test::find_last_event(events, moex::twime_trade::TwimeSessionEventType::RetransmissionReceived) != nullptr,
            "Retransmission event missing");
        moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 6,
                                       "retransmission did not restore expected inbound sequence");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
