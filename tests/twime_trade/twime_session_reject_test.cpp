#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "establishment_reject_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);
            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});

            auto reject = moex::twime_trade::test::make_request("EstablishmentReject");
            moex::twime_trade::test::script_message(transport, reject);
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Rejected,
                                                   "EstablishmentReject did not transition session to Rejected");
            auto events = session.drain_events();
            moex::twime_sbe::test::require(
                moex::twime_trade::test::find_last_event(
                    events, moex::twime_trade::TwimeSessionEventType::EstablishmentRejected) != nullptr,
                "EstablishmentRejected event missing");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "session_reject_test";
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

            auto session_reject = moex::twime_trade::test::make_request("SessionReject");
            moex::twime_trade::test::script_message(transport, session_reject);
            session.poll_transport();

            auto events = session.drain_events();
            moex::twime_sbe::test::require(
                moex::twime_trade::test::find_last_event(
                    events, moex::twime_trade::TwimeSessionEventType::SessionRejectReceived) != nullptr,
                "SessionReject event missing");
            moex::twime_sbe::test::require(session.state() == moex::twime_trade::TwimeSessionState::Active,
                                           "SessionReject should not change state away from Active");
            moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 11,
                                           "SessionReject must not consume application sequence state");
            moex::twime_sbe::test::require(!session.inbound_journal().last_n(1).front().recoverable,
                                           "SessionReject must not be marked recoverable");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
