#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "keepalive_override_test";
            config.keepalive_interval_ms = 1000;
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
            auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
            for (auto& field : ack.fields) {
                if (field.name == "KeepaliveInterval") {
                    field.value = moex::twime_sbe::TwimeFieldValue::delta_millisecs(2500);
                }
            }
            moex::twime_trade::test::script_message(transport, ack);
            session.poll_transport();

            moex::twime_sbe::test::require(session.active_keepalive_interval_ms() == 2500,
                                           "EstablishmentAck did not override active keepalive interval");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "keepalive_invalid_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
            auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
            for (auto& field : ack.fields) {
                if (field.name == "KeepaliveInterval") {
                    field.value = moex::twime_sbe::TwimeFieldValue::delta_millisecs(999);
                }
            }
            moex::twime_trade::test::script_message(transport, ack);
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Faulted,
                                                   "invalid KeepaliveInterval should fault session");
            const auto events = session.drain_events();
            moex::twime_sbe::test::require(
                moex::twime_trade::test::find_last_event(
                    events, moex::twime_trade::TwimeSessionEventType::KeepaliveIntervalRejected) != nullptr,
                "invalid KeepaliveInterval event missing");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
