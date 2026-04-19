#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

namespace {

void establish_active_session(moex::twime_trade::TwimeSession& session,
                              moex::twime_trade::TwimeFakeTransport& transport) {
    session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
    auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
    for (auto& field : ack.fields) {
        if (field.name == "NextSeqNo") {
            field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(11);
        }
    }
    moex::twime_trade::test::script_message(transport, ack);
    session.poll_transport();
    static_cast<void>(session.drain_events());
}

void open_gap_and_request_retransmission(moex::twime_trade::TwimeSession& session,
                                         moex::twime_trade::TwimeFakeTransport& transport) {
    auto sequence = moex::twime_trade::test::make_request("Sequence");
    for (auto& field : sequence.fields) {
        if (field.name == "NextSeqNo") {
            field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(16);
        }
    }
    moex::twime_trade::test::script_message(transport, sequence);
    session.poll_transport();
    static_cast<void>(session.drain_events());
}

moex::twime_sbe::TwimeEncodeRequest make_retransmission_metadata() {
    auto retransmission = moex::twime_trade::test::make_request("Retransmission");
    for (auto& field : retransmission.fields) {
        if (field.name == "NextSeqNo") {
            field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(16);
        }
        if (field.name == "Count") {
            field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(5);
        }
    }
    return retransmission;
}

} // namespace

int main() {
    try {
        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "retransmission_metadata_only_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            establish_active_session(session, transport);
            open_gap_and_request_retransmission(session, transport);

            auto retransmission = make_retransmission_metadata();
            moex::twime_trade::test::script_message(transport, retransmission);
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Recovering,
                                                   "Retransmission metadata alone must not restore Active state");
            auto events = session.drain_events();
            moex::twime_sbe::test::require(
                moex::twime_trade::test::find_last_event(
                    events, moex::twime_trade::TwimeSessionEventType::RetransmissionReceived) != nullptr,
                "Retransmission event missing");
            moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 11,
                                           "metadata alone must not advance expected inbound sequence");
            moex::twime_sbe::test::require(!session.inbound_journal().last_n(1).front().recoverable,
                                           "Retransmission metadata must not be marked recoverable");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "retransmission_completes_after_messages_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            establish_active_session(session, transport);
            open_gap_and_request_retransmission(session, transport);

            auto retransmission = make_retransmission_metadata();
            moex::twime_trade::test::script_message(transport, retransmission);
            for (std::uint64_t sequence_number = 11; sequence_number <= 15; ++sequence_number) {
                auto response = moex::twime_trade::test::make_request("NewOrderSingleResponse");
                moex::twime_trade::test::script_message(transport, response, sequence_number);
            }
            session.poll_transport();

            moex::twime_trade::test::require_state(
                session.state(), moex::twime_trade::TwimeSessionState::Active,
                "expected retransmitted application messages must restore Active state");
            moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 16,
                                           "retransmitted application messages must advance expected inbound sequence");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "retransmission_count_mismatch_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            establish_active_session(session, transport);
            open_gap_and_request_retransmission(session, transport);

            auto retransmission = make_retransmission_metadata();
            for (auto& field : retransmission.fields) {
                if (field.name == "Count") {
                    field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(4);
                }
            }
            moex::twime_trade::test::script_message(transport, retransmission);
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Faulted,
                                                   "Retransmission count mismatch must fault fake session");
            const auto events = session.drain_events();
            moex::twime_sbe::test::require(
                moex::twime_trade::test::find_last_event(
                    events, moex::twime_trade::TwimeSessionEventType::RetransmissionProtocolViolation) != nullptr,
                "Retransmission protocol violation event missing for count mismatch");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "unexpected_retransmission_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            establish_active_session(session, transport);

            auto retransmission = make_retransmission_metadata();
            moex::twime_trade::test::script_message(transport, retransmission);
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Faulted,
                                                   "Retransmission without pending request must fault fake session");
            const auto events = session.drain_events();
            moex::twime_sbe::test::require(
                moex::twime_trade::test::find_last_event(
                    events, moex::twime_trade::TwimeSessionEventType::RetransmissionProtocolViolation) != nullptr,
                "Retransmission protocol violation event missing for unexpected retransmission");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
