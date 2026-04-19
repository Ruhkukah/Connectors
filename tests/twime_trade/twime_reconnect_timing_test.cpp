#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "reconnect_timing_test";
        moex::twime_trade::TwimeFakeTransport transport;
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        auto reject = moex::twime_trade::test::make_request("EstablishmentReject");
        moex::twime_trade::test::script_message(transport, reject);
        session.poll_transport();
        static_cast<void>(session.drain_events());

        const auto outbound_before = session.outbound_journal().size();
        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        moex::twime_sbe::test::require(session.outbound_journal().size() == outbound_before,
                                       "too-fast reconnect must not send a second Establish");
        const auto too_fast_events = session.drain_events();
        moex::twime_sbe::test::require(
            moex::twime_trade::test::find_last_event(
                too_fast_events, moex::twime_trade::TwimeSessionEventType::ReconnectTooFast) != nullptr,
            "ReconnectTooFast event missing");

        clock.advance(1000);
        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        moex::twime_sbe::test::require(session.outbound_journal().size() == outbound_before + 1,
                                       "reconnect after >=1000ms must send another Establish");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
