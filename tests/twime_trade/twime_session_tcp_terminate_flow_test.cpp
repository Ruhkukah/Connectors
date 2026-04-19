#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_session_tcp_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::test::LocalTcpServer server;
        moex::twime_trade::TwimeSessionConfig session_config;
        session_config.session_id = "tcp_terminate_flow_test";

        moex::twime_trade::transport::TwimeTcpTransport transport(
            moex::twime_trade::test::make_local_tcp_config(server.port()));
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(session_config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        static_cast<void>(moex::twime_trade::test::wait_for_establish(server, session, session_config, clock));
        moex::twime_trade::test::complete_establish_handshake(session, server);
        static_cast<void>(session.drain_events());

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::SendTerminate});
        const auto terminate_bytes = server.receive_up_to(512, std::chrono::milliseconds(3000));
        const auto decoded = moex::twime_trade::test::decode_streamed_frames(terminate_bytes);
        moex::twime_sbe::test::require(decoded.size() == 1, "session did not emit a single TCP Terminate frame");
        moex::twime_sbe::test::require(decoded.front().metadata->name == "Terminate",
                                       "session did not emit Terminate over TCP");

        auto terminate = moex::twime_trade::test::make_request("Terminate");
        for (auto& field : terminate.fields) {
            if (field.name == "TerminationCode") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(0);
            }
        }
        server.send_bytes(moex::twime_trade::test::encode_bytes(terminate));
        moex::twime_trade::test::pump_session_until(session, [](const moex::twime_trade::TwimeSession& candidate) {
            return candidate.state() == moex::twime_trade::TwimeSessionState::Terminated;
        });

        server.close_client();
        bool saw_clean_close = false;
        for (int attempt = 0; attempt < 128 && !saw_clean_close; ++attempt) {
            session.poll_transport();
            const auto events = session.drain_events();
            saw_clean_close =
                moex::twime_trade::test::find_last_event(
                    events, moex::twime_trade::TwimeSessionEventType::PeerClosedCleanAfterTerminateResponse) != nullptr;
        }
        moex::twime_sbe::test::require(saw_clean_close, "TCP terminate flow did not emit clean peer close event");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
