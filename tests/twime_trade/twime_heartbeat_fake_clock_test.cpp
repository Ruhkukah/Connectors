#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "heartbeat_test";
        config.heartbeat_interval_ms = 250;
        moex::twime_trade::TwimeFakeTransport transport;
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
        moex::twime_trade::test::script_message(transport, ack);
        session.poll_transport();
        static_cast<void>(session.drain_events());

        clock.advance(249);
        session.on_timer_tick();
        moex::twime_sbe::test::require(session.outbound_journal().size() == 1,
                                       "heartbeat sent before fake clock deadline");

        clock.advance(1);
        session.on_timer_tick();
        moex::twime_sbe::test::require(session.outbound_journal().size() == 2,
                                       "heartbeat not sent after fake clock deadline");
        moex::twime_sbe::test::require(session.outbound_journal().last_n(1).front().message_name == "Sequence",
                                       "last outbound message is not Sequence heartbeat");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
