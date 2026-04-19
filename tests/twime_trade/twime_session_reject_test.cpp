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
                moex::twime_trade::test::find_last_event(events, moex::twime_trade::TwimeSessionEventType::EstablishmentRejected) != nullptr,
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
            moex::twime_trade::test::script_message(transport, ack);
            session.poll_transport();

            auto session_reject = moex::twime_trade::test::make_request("SessionReject");
            moex::twime_trade::test::script_message(transport, session_reject);
            session.poll_transport();

            auto events = session.drain_events();
            moex::twime_sbe::test::require(
                moex::twime_trade::test::find_last_event(events, moex::twime_trade::TwimeSessionEventType::SessionRejectReceived) != nullptr,
                "SessionReject event missing");
            moex::twime_sbe::test::require(session.state() == moex::twime_trade::TwimeSessionState::Active,
                                           "SessionReject should not change state away from Active");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
