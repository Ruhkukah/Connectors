#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "business_reject_test";
        moex::twime_trade::TwimeFakeTransport transport;
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
        moex::twime_trade::test::script_message(transport, ack);
        session.poll_transport();
        static_cast<void>(session.drain_events());

        auto reject = moex::twime_trade::test::make_request("BusinessMessageReject");
        for (auto& field : reject.fields) {
            if (field.name == "OrdRejReason") {
                field.value = moex::twime_sbe::TwimeFieldValue::signed_integer(-12);
            }
        }
        moex::twime_trade::test::script_message(transport, reject);
        session.poll_transport();

        auto events = session.drain_events();
        const auto* event =
            moex::twime_trade::test::find_last_event(events, moex::twime_trade::TwimeSessionEventType::BusinessRejectReceived);
        moex::twime_sbe::test::require(event != nullptr, "BusinessMessageReject event missing");
        moex::twime_sbe::test::require(event->reason_code == -12, "BusinessMessageReject reason code mismatch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
