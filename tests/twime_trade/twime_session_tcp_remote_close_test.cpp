#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_session_tcp_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::test::LocalTcpServer server;
        moex::twime_trade::TwimeSessionConfig session_config;
        session_config.session_id = "tcp_remote_close_test";

        moex::twime_trade::transport::TwimeTcpTransport transport(
            moex::twime_trade::test::make_local_tcp_config(server.port()));
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(session_config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        static_cast<void>(moex::twime_trade::test::wait_for_establish(server, session, session_config, clock));
        moex::twime_trade::test::complete_establish_handshake(session, server);
        static_cast<void>(session.drain_events());

        server.close_client();
        moex::twime_trade::test::pump_session_until(session, [](const moex::twime_trade::TwimeSession& candidate) {
            return candidate.state() == moex::twime_trade::TwimeSessionState::Faulted;
        });
        const auto events = session.drain_events();
        moex::twime_sbe::test::require(moex::twime_trade::test::find_last_event(
                                           events, moex::twime_trade::TwimeSessionEventType::PeerClosed) != nullptr,
                                       "TCP session remote close did not emit PeerClosed");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
