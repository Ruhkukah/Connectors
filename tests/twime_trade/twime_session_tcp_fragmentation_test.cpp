#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_trade/transport/twime_tcp_transport.hpp"

#include "twime_session_tcp_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::test::LocalTcpServer server;
        moex::twime_trade::TwimeSessionConfig session_config;
        session_config.session_id = "tcp_fragmentation_test";

        auto transport_config = moex::twime_trade::test::make_local_tcp_config(server.port());
        transport_config.buffer_policy.read_chunk_bytes = 2;
        moex::twime_trade::transport::TwimeTcpTransport transport(transport_config);
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(session_config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        static_cast<void>(moex::twime_trade::test::wait_for_establish(server, session, session_config, clock));
        moex::twime_trade::test::complete_establish_handshake(session, server, 11, 1);

        moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Active,
                                               "fragmented TCP EstablishmentAck did not activate session");
        moex::twime_sbe::test::require(transport.metrics().partial_read_events > 0,
                                       "fragmented TCP path did not record partial reads");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
