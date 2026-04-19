#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "gap_test";
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

        moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Recovering,
                                               "gap did not transition session to Recovering");
        auto events = session.drain_events();
        const auto* gap_event =
            moex::twime_trade::test::find_last_event(events, moex::twime_trade::TwimeSessionEventType::SequenceGapDetected);
        moex::twime_sbe::test::require(gap_event != nullptr, "gap event missing");
        moex::twime_sbe::test::require(gap_event->gap_from == 1 && gap_event->gap_to == 5,
                                       "gap boundaries are not deterministic");
        moex::twime_sbe::test::require(session.outbound_journal().last_n(1).front().message_name == "RetransmitRequest",
                                       "gap did not trigger a RetransmitRequest");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
