#include "moex/twime_trade/twime_session.hpp"

#include "twime_trade_test_support.hpp"

#include <iostream>

int main() {
    try {
        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "establish_test";
            config.keepalive_interval_ms = 2000;
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
            moex::twime_sbe::test::require(session.state() == moex::twime_trade::TwimeSessionState::Establishing,
                                           "session did not enter Establishing");
            moex::twime_sbe::test::require(session.outbound_journal().size() == 1, "Establish was not journaled");

            const auto establish =
                moex::twime_trade::test::decode_bytes(session.outbound_journal().last_n(1).front().bytes);
            const auto* keepalive_field = moex::twime_trade::test::find_field(establish, "KeepaliveInterval");
            moex::twime_sbe::test::require(keepalive_field != nullptr, "Establish KeepaliveInterval missing");
            moex::twime_sbe::test::require(keepalive_field->value.unsigned_value == 2000,
                                           "Establish KeepaliveInterval mismatch");

            auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
            for (auto& field : ack.fields) {
                if (field.name == "KeepaliveInterval") {
                    field.value = moex::twime_sbe::TwimeFieldValue::delta_millisecs(1500);
                }
                if (field.name == "NextSeqNo") {
                    field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(11);
                }
            }
            moex::twime_trade::test::script_message(transport, ack);
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Active,
                                                   "session did not become Active after EstablishmentAck");
            moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 11,
                                           "inbound sequence was not initialized from EstablishmentAck");
            moex::twime_sbe::test::require(session.sequence_state().next_outbound_seq() == 1,
                                           "EstablishmentAck must not change outbound sequence state");
            moex::twime_sbe::test::require(session.active_keepalive_interval_ms() == 1500,
                                           "acknowledged KeepaliveInterval did not become active");
            moex::twime_sbe::test::require(!session.cert_log_lines().empty(), "cert log lines were not recorded");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "dirty_recovery_establish_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            recovery_store.save(config.session_id, {
                                                       .next_outbound_seq = 7,
                                                       .next_expected_inbound_seq = 11,
                                                       .last_establishment_id = 1'715'000'000'000'000'001ULL,
                                                       .recovery_epoch = 3,
                                                       .last_clean_shutdown = false,
                                                   });
            moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
            moex::twime_trade::TwimeSession session(config, transport, recovery_store, clock);

            session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});

            auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
            for (auto& field : ack.fields) {
                if (field.name == "NextSeqNo") {
                    field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(14);
                }
            }
            moex::twime_trade::test::script_message(transport, ack);
            session.poll_transport();

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Recovering,
                                                   "dirty restart should remain Recovering until retransmission");
            moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 11,
                                           "dirty restart must not overwrite persisted expected inbound sequence");
            moex::twime_sbe::test::require(session.sequence_state().next_outbound_seq() == 7,
                                           "EstablishmentAck must not overwrite outbound sequence on dirty restart");

            const auto last = session.outbound_journal().last_n(1).front();
            moex::twime_sbe::test::require(last.message_name == "RetransmitRequest",
                                           "dirty restart did not request retransmission");
            moex::twime_sbe::test::require(last.cert_log_line.find("FromSeqNo=11") != std::string::npos,
                                           "retransmission request does not start from persisted expected inbound");
            moex::twime_sbe::test::require(last.cert_log_line.find("Count=3") != std::string::npos,
                                           "retransmission request count mismatch after EstablishmentAck");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "dirty_recovery_equal_ack_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            recovery_store.save(config.session_id, {
                                                       .next_outbound_seq = 9,
                                                       .next_expected_inbound_seq = 11,
                                                       .last_establishment_id = 1'715'000'000'000'000'002ULL,
                                                       .recovery_epoch = 4,
                                                       .last_clean_shutdown = false,
                                                   });
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

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Active,
                                                   "equal EstablishmentAck.NextSeqNo should resume Active");
            moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 11,
                                           "equal EstablishmentAck.NextSeqNo must keep expected inbound sequence");
            moex::twime_sbe::test::require(session.outbound_journal().last_n(1).front().message_name == "Establish",
                                           "equal EstablishmentAck.NextSeqNo must not send RetransmitRequest");
        }

        {
            moex::twime_trade::TwimeSessionConfig config;
            config.session_id = "dirty_recovery_counter_reset_test";
            moex::twime_trade::TwimeFakeTransport transport;
            moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
            recovery_store.save(config.session_id, {
                                                       .next_outbound_seq = 12,
                                                       .next_expected_inbound_seq = 21,
                                                       .last_establishment_id = 1'715'000'000'000'000'003ULL,
                                                       .recovery_epoch = 5,
                                                       .last_clean_shutdown = false,
                                                   });
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

            moex::twime_trade::test::require_state(session.state(), moex::twime_trade::TwimeSessionState::Active,
                                                   "counter reset EstablishmentAck should resume Active");
            moex::twime_sbe::test::require(session.sequence_state().next_expected_inbound_seq() == 11,
                                           "counter reset must reset expected inbound sequence");
            const auto events = session.drain_events();
            const auto* reset_event = moex::twime_trade::test::find_last_event(
                events, moex::twime_trade::TwimeSessionEventType::MessageCounterResetDetected);
            moex::twime_sbe::test::require(reset_event != nullptr, "message counter reset event missing");
            moex::twime_sbe::test::require(reset_event->gap_from == 21 && reset_event->gap_to == 11,
                                           "message counter reset event did not capture old/new counters");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
