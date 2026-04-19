#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_trade::TwimeSessionConfig config;
        config.session_id = "message_layer_rules_test";
        moex::twime_trade::TwimeFakeTransport transport;
        moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
        moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
        moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

        session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
        auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
        for (auto& field : ack.fields) {
            if (field.name == "NextSeqNo") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(11);
            }
        }
        moex::twime_trade::test::script_message(transport, ack);
        session.poll_transport();
        moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 11,
                                       "EstablishmentAck should initialize inbound counter");
        moex::twime_sbe::test::require(!session.inbound_journal().last_n(1).front().recoverable,
                                       "EstablishmentAck must not be recoverable");
        static_cast<void>(session.drain_events());

        auto sequence = moex::twime_trade::test::make_request("Sequence");
        for (auto& field : sequence.fields) {
            if (field.name == "NextSeqNo") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(11);
            }
        }
        moex::twime_trade::test::script_message(transport, sequence);
        session.poll_transport();
        moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 11,
                                       "Sequence must not consume application inbound sequence");
        moex::twime_sbe::test::require(!session.inbound_journal().last_n(1).front().recoverable,
                                       "Sequence must not be recoverable");

        auto new_order_response = moex::twime_trade::test::make_request("NewOrderSingleResponse");
        moex::twime_trade::test::script_message(transport, new_order_response, 11);
        session.poll_transport();
        moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 12,
                                       "NewOrderSingleResponse must consume application inbound sequence");
        moex::twime_sbe::test::require(session.inbound_journal().last_n(1).front().recoverable,
                                       "NewOrderSingleResponse should be marked recoverable");

        auto execution_report = moex::twime_trade::test::make_request("ExecutionSingleReport");
        moex::twime_trade::test::script_message(transport, execution_report, 12);
        session.poll_transport();
        moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 13,
                                       "ExecutionSingleReport must consume application inbound sequence");
        moex::twime_sbe::test::require(session.inbound_journal().last_n(1).front().recoverable,
                                       "ExecutionSingleReport should be marked recoverable");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
