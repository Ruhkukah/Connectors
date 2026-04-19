#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "normal_retransmit_limit_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
            auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
            moex::twime_trade::test::script_message(transport, ack);
            session.poll_transport();
            static_cast<void>(session.drain_events());

            session.apply_command({
                .type = moex::twime_trade::TwimeSessionCommandType::RequestRetransmit,
                .from_seq_no = 31,
                .count = 11,
            });
            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Faulted,
                                                   "normal session recovery must reject retransmit count > 10");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "full_recovery_retransmit_limit_test";
            config.recovery_mode = moex::twime_trade::TwimeRecoveryMode::FullRecoveryService;
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
            auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
            moex::twime_trade::test::script_message(transport, ack);
            session.poll_transport();
            static_cast<void>(session.drain_events());

            session.apply_command({
                .type = moex::twime_trade::TwimeSessionCommandType::RequestRetransmit,
                .from_seq_no = 31,
                .count = 1000,
            });
            moex::twime_sbe::test::require(session.state() == moex::twime_trade::TwimeSessionState::Active,
                                           "full recovery mode should allow retransmit count 1000");
            moex::twime_sbe::test::require(session.outbound_journal().last_n(1).front().message_name ==
                                               "RetransmitRequest",
                                           "full recovery mode did not send RetransmitRequest");

            session.apply_command({
                .type = moex::twime_trade::TwimeSessionCommandType::RequestRetransmit,
                .from_seq_no = 31,
                .count = 1001,
            });
            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Faulted,
                                                   "full recovery mode must reject retransmit count > 1000");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
