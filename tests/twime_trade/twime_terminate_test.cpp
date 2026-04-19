#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "terminate_test";
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

        transport.script_peer_close();
        session.poll_transport();
        moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Terminated,
                                               "session did not become Terminated after fake peer close");
        moex::twime_sbe::test::require(session.recovery_state().last_clean_shutdown,
                                       "clean shutdown flag was not persisted");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
