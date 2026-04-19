#include "moex/twime_trade/twime_session_health.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        using namespace moex::twime_trade;

        TwimeFakeTransport transport;
        TwimeInMemoryRecoveryStateStore recovery_store;
        TwimeFakeClock clock(1000);
        TwimeSessionConfig config;
        config.credentials = "LOGIN";

        TwimeSession session(config, transport, recovery_store, clock);
        session.apply_command({.type = TwimeSessionCommandType::ConnectFake});
        auto events = session.drain_events();

        moex::twime_trade::test::script_message(transport, moex::twime_trade::test::make_request("EstablishmentAck"));
        session.poll_transport();
        auto more_events = session.drain_events();
        events.insert(events.end(), more_events.begin(), more_events.end());

        TwimeSessionMetrics metrics;
        update_twime_session_metrics(metrics, events, clock.now_ms());
        transport::TwimeTransportMetrics transport_metrics;
        transport_metrics.bytes_read = 64;
        transport_metrics.bytes_written = 96;

        const auto snapshot = make_twime_session_health_snapshot(session, true, transport_metrics, metrics);
        moex::twime_sbe::test::require(snapshot.transport_open, "health snapshot must report open transport");
        moex::twime_sbe::test::require(snapshot.state == TwimeSessionState::Active,
                                       "health snapshot must report active state after ack");
        moex::twime_sbe::test::require(snapshot.active_keepalive_interval_ms > 0,
                                       "health snapshot must expose active keepalive");
        moex::twime_sbe::test::require(snapshot.bytes_read == 64 && snapshot.bytes_written == 96,
                                       "health snapshot must expose transport byte counters");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
