#include "moex/twime_trade/twime_rate_limit_model.hpp"
#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_sbe/twime_schema.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        {
            const auto* new_order = moex::twime_sbe::TwimeSchemaView::find_message_by_name("NewOrderSingle");
            const auto* cancel = moex::twime_sbe::TwimeSchemaView::find_message_by_name("OrderCancelRequest");
            const auto* replace = moex::twime_sbe::TwimeSchemaView::find_message_by_name("OrderReplaceRequest");
            const auto* mass_cancel = moex::twime_sbe::TwimeSchemaView::find_message_by_name("OrderMassCancelRequest");
            const auto* sequence = moex::twime_sbe::TwimeSchemaView::find_message_by_name("Sequence");
            moex::twime_sbe::test::require(new_order != nullptr && cancel != nullptr && replace != nullptr &&
                                               mass_cancel != nullptr && sequence != nullptr,
                                           "missing TWIME message metadata for rate classification test");

            const auto new_order_class = moex::twime_trade::classify_outbound_message(*new_order);
            const auto cancel_class = moex::twime_trade::classify_outbound_message(*cancel);
            const auto replace_class = moex::twime_trade::classify_outbound_message(*replace);
            const auto mass_cancel_class = moex::twime_trade::classify_outbound_message(*mass_cancel);
            const auto sequence_class = moex::twime_trade::classify_outbound_message(*sequence);

            moex::twime_sbe::test::require(new_order_class.counts_trading_rate, "NewOrderSingle must count as trading");
            moex::twime_sbe::test::require(cancel_class.counts_trading_rate,
                                           "OrderCancelRequest must count as trading");
            moex::twime_sbe::test::require(replace_class.counts_trading_rate,
                                           "OrderReplaceRequest must count as trading");
            moex::twime_sbe::test::require(mass_cancel_class.counts_trading_rate,
                                           "OrderMassCancelRequest must count as trading");
            moex::twime_sbe::test::require(!sequence_class.counts_trading_rate,
                                           "Sequence heartbeat must not count as trading");
            moex::twime_sbe::test::require(sequence_class.counts_total_rate,
                                           "Sequence heartbeat must still count toward total outbound rate");

            moex::twime_trade::TwimeRateLimitModel model(100, 2, 3, 1000);
            const auto decision1 = model.observe_send(0, new_order_class);
            const auto decision2 = model.observe_send(0, cancel_class);
            const auto decision3 = model.observe_send(0, sequence_class);
            moex::twime_sbe::test::require(decision1.allowed && decision2.allowed && decision3.allowed,
                                           "expected first three model decisions to be allowed");
            moex::twime_sbe::test::require(decision3.trading_used_in_window == 2,
                                           "Sequence heartbeat must not increment trading usage");
            moex::twime_sbe::test::require(decision3.total_used_in_window == 3,
                                           "total outbound rate must include Sequence heartbeat");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "heartbeat_rate_limit_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
            auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
            moex::twime_trade::test::script_message(transport, ack);
            session.poll_transport();
            static_cast<void>(session.drain_events());

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::SendHeartbeat});
            session.apply_command({moex::twime_trade::TwimeSessionCommandType::SendHeartbeat});
            session.apply_command({moex::twime_trade::TwimeSessionCommandType::SendHeartbeat});
            moex::twime_sbe::test::require(session.state() == moex::twime_trade::TwimeSessionState::Active,
                                           "three heartbeats per second should be accepted");

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::SendHeartbeat});
            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Faulted,
                                                   "fourth heartbeat in one second must fault fake session");
            const auto events = session.drain_events();
            moex::twime_sbe::test::require(
                moex::twime_trade::test::find_last_event(
                    events, moex::twime_trade::TwimeSessionEventType::HeartbeatRateViolation) != nullptr,
                "heartbeat rate violation event missing");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
