#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>
#include <limits>

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
            config.session_id = "huge_gap_retransmit_limit_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
            auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
            moex::twime_trade::test::script_message(transport, ack);
            session.poll_transport();
            static_cast<void>(session.drain_events());

            auto sequence = moex::twime_trade::test::make_request("Sequence");
            for (auto& field : sequence.fields) {
                if (field.name == "NextSeqNo") {
                    field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(
                        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 100ULL);
                }
            }
            moex::twime_trade::test::script_message(transport, sequence);
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Faulted,
                                                   "huge gap must be rejected without uint32 overflow");
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

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "retransmit_range_overflow_test";
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
                .from_seq_no = std::numeric_limits<std::uint64_t>::max(),
                .count = 2,
            });
            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Faulted,
                                                   "from_seq_no + count - 1 overflow must be rejected");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
