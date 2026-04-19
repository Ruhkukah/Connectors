#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_session_tcp_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::test::LocalTcpServer server;
        moex::twime_trade::TwimeSessionConfig session_config;
        session_config.session_id = "tcp_establish_test";

        moex::twime_trade::transport::TwimeTcpTransport transport(
            moex::twime_trade::test::make_local_tcp_config(server.port()));
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(session_config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        const auto establish_bytes =
            moex::twime_trade::test::wait_for_establish(server, session, session_config, clock);
        const auto decoded = moex::twime_trade::test::decode_streamed_frames(establish_bytes);
        moex::twime_sbe::test::require(decoded.size() == 1, "TCP session establish bytes did not decode to one frame");
        moex::twime_sbe::test::require(decoded.front().metadata->name == "Establish",
                                       "TCP session did not emit Establish first");

        moex::twime_trade::test::complete_establish_handshake(session, server);
        moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Active,
                                               "TCP session did not become Active");
        moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 11,
                                       "TCP EstablishmentAck did not initialize inbound sequence");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
