#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "flood_reject_test";
        moex::twime_trade::TwimeFakeTransport transport;
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
        moex::twime_trade::test::script_message(transport, ack);
        session.poll_transport();
        static_cast<void>(session.drain_events());

        auto flood = moex::twime_trade::test::make_request("FloodReject");
        moex::twime_trade::test::script_message(transport, flood);
        session.poll_transport();

        auto events = session.drain_events();
        const auto* event = moex::twime_trade::test::find_last_event(
            events, moex::twime_trade::TwimeSessionEventType::FloodRejectReceived);
        moex::twime_sbe::test::require(event != nullptr, "FloodReject event missing");
        moex::twime_sbe::test::require(event->cert_log_line.find("FloodReject") != std::string::npos,
                                       "FloodReject cert log missing");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
