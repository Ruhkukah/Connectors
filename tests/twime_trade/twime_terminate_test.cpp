#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "terminate_requires_inbound_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
            auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
            moex::twime_trade::test::script_message(transport, ack);
            session.poll_transport();
            static_cast<void>(session.drain_events());

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::SendTerminate});
            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Terminating,
                                                   "session did not enter Terminating");
            moex::twime_sbe::test::require(session.outbound_journal().last_n(1).front().message_name == "Terminate",
                                           "Terminate message was not sent");
            moex::twime_sbe::test::require(!session.recovery_state().last_clean_shutdown,
                                           "clean shutdown flag must not be set before inbound Terminate");

            transport.script_peer_close();
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Faulted,
                                                   "peer close without inbound Terminate should fault the session");
            moex::twime_sbe::test::require(!session.recovery_state().last_clean_shutdown,
                                           "peer close without inbound Terminate must not be clean shutdown");
            const auto events = session.drain_events();
            moex::twime_sbe::test::require(
                moex::twime_trade::test::find_last_event(
                    events, moex::twime_trade::TwimeSessionEventType::PeerClosedUnexpectedWhileTerminating) != nullptr,
                "missing unexpected peer-close event while Terminating");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "terminate_finished_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
            auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
            moex::twime_trade::test::script_message(transport, ack);
            session.poll_transport();
            static_cast<void>(session.drain_events());

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::SendTerminate});
            auto terminate = moex::twime_trade::test::make_request("Terminate");
            moex::twime_trade::test::script_message(transport, terminate);
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Terminated,
                                                   "inbound Terminate(Finished) did not terminate session cleanly");
            moex::twime_sbe::test::require(session.recovery_state().last_clean_shutdown,
                                           "inbound Terminate(Finished) must mark clean shutdown");

            transport.script_peer_close();
            session.poll_transport();
            const auto events = session.drain_events();
            moex::twime_sbe::test::require(
                moex::twime_trade::test::find_last_event(
                    events, moex::twime_trade::TwimeSessionEventType::PeerClosedCleanAfterTerminateResponse) != nullptr,
                "clean peer-close event after inbound Terminate(Finished) missing");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
