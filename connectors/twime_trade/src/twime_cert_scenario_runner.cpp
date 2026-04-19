#include "moex/twime_trade/twime_cert_scenario_runner.hpp"

namespace moex::twime_trade {

namespace {

using moex::twime_sbe::TwimeEncodeRequest;
using moex::twime_sbe::TwimeFieldInput;
using moex::twime_sbe::TwimeFieldValue;

TwimeEncodeRequest request(std::string_view name, std::uint16_t template_id,
                           std::initializer_list<TwimeFieldInput> fields = {}) {
    TwimeEncodeRequest out;
    out.message_name = std::string(name);
    out.template_id = template_id;
    out.fields.assign(fields.begin(), fields.end());
    return out;
}

TwimeSessionConfig default_config() {
    TwimeSessionConfig config;
    config.session_id = "twime_phase2b";
    config.credentials = "LOGIN";
    config.keepalive_interval_ms = 1000;
    return config;
}

TwimeCertScenarioAction establish_ack_action(std::uint64_t next_seq_no = 11, std::uint32_t keepalive_ms = 1000) {
    return {
        .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
        .message = request("EstablishmentAck", 5001,
                           {
                               {"RequestTimestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'100ULL)},
                               {"KeepaliveInterval", TwimeFieldValue::delta_millisecs(keepalive_ms)},
                               {"NextSeqNo", TwimeFieldValue::unsigned_integer(next_seq_no)},
                           }),
    };
}

TwimeCertScenarioAction inbound_terminate_finished_action() {
    return {
        .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
        .message = request("Terminate", 5003, {{"TerminationCode", TwimeFieldValue::enum_name("Finished")}}),
    };
}

TwimeCertScenario session_establish() {
    return {
        .scenario_id = "twime_session_establish",
        .title = "Synthetic TWIME session establish",
        .config = default_config(),
        .actions = {{.kind = TwimeCertScenarioActionKind::Connect}, establish_ack_action(11, 1000)},
        .expected_final_state = TwimeSessionState::Active,
    };
}

TwimeCertScenario session_establish_ack_sets_inbound_counter() {
    return {
        .scenario_id = "twime_session_establish_ack_sets_inbound_counter",
        .title = "Synthetic TWIME EstablishmentAck sets inbound counter",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1200),
            },
        .expected_final_state = TwimeSessionState::Active,
    };
}

TwimeCertScenario session_reject() {
    return {
        .scenario_id = "twime_session_reject",
        .title = "Synthetic TWIME session reject",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                {
                    .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                    .message =
                        request("EstablishmentReject", 5002,
                                {
                                    {"RequestTimestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'100ULL)},
                                    {"EstablishmentRejectCode", TwimeFieldValue::enum_name("Credentials")},
                                }),
                },
            },
        .expected_final_state = TwimeSessionState::Rejected,
    };
}

TwimeCertScenario heartbeat_sequence() {
    return {
        .scenario_id = "twime_heartbeat_sequence",
        .title = "Synthetic TWIME heartbeat sequence",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1000),
                {.kind = TwimeCertScenarioActionKind::AdvanceClock, .advance_ms = 1000},
                {.kind = TwimeCertScenarioActionKind::TimerTick},
                {
                    .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                    .message = request("Sequence", 5006, {{"NextSeqNo", TwimeFieldValue::unsigned_integer(16)}}),
                },
            },
        .expected_final_state = TwimeSessionState::Recovering,
    };
}

TwimeCertScenario client_sequence_heartbeat_null_nextseqno() {
    return {
        .scenario_id = "twime_client_sequence_heartbeat_null_nextseqno",
        .title = "Synthetic TWIME client Sequence heartbeat uses null NextSeqNo",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1000),
                {.kind = TwimeCertScenarioActionKind::CommandHeartbeat},
            },
        .expected_final_state = TwimeSessionState::Active,
    };
}

TwimeCertScenario terminate() {
    return {
        .scenario_id = "twime_terminate",
        .title = "Synthetic TWIME terminate",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1000),
                {.kind = TwimeCertScenarioActionKind::CommandTerminate},
                inbound_terminate_finished_action(),
                {.kind = TwimeCertScenarioActionKind::InjectPeerClose},
            },
        .expected_final_state = TwimeSessionState::Terminated,
    };
}

TwimeCertScenario terminate_requires_inbound_terminate() {
    return {
        .scenario_id = "twime_terminate_requires_inbound_terminate",
        .title = "Synthetic TWIME terminate requires inbound Terminate",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1000),
                {.kind = TwimeCertScenarioActionKind::CommandTerminate},
                {.kind = TwimeCertScenarioActionKind::InjectPeerClose},
            },
        .expected_final_state = TwimeSessionState::Faulted,
    };
}

TwimeCertScenario retransmit_last5() {
    return {
        .scenario_id = "twime_retransmit_last5",
        .title = "Synthetic TWIME retransmit last 5",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1000),
                {
                    .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                    .message = request("Sequence", 5006, {{"NextSeqNo", TwimeFieldValue::unsigned_integer(16)}}),
                },
                {
                    .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                    .message =
                        request("Retransmission", 5005,
                                {
                                    {"NextSeqNo", TwimeFieldValue::unsigned_integer(16)},
                                    {"RequestTimestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'200ULL)},
                                    {"Count", TwimeFieldValue::unsigned_integer(5)},
                                }),
                },
            },
        .expected_final_state = TwimeSessionState::Active,
    };
}

TwimeCertScenario normal_retransmit_limit_10() {
    return {
        .scenario_id = "twime_normal_retransmit_limit_10",
        .title = "Synthetic TWIME normal retransmit limit 10",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1000),
                {.kind = TwimeCertScenarioActionKind::CommandRetransmit,
                 .retransmit_from_seq_no = 21,
                 .retransmit_count = 10},
            },
        .expected_final_state = TwimeSessionState::Active,
    };
}

TwimeCertScenario full_recovery_retransmit_limit_1000() {
    auto config = default_config();
    config.recovery_mode = TwimeRecoveryMode::FullRecoveryService;
    return {
        .scenario_id = "twime_full_recovery_retransmit_limit_1000",
        .title = "Synthetic TWIME full recovery retransmit limit 1000",
        .config = config,
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1000),
                {
                    .kind = TwimeCertScenarioActionKind::CommandRetransmit,
                    .retransmit_from_seq_no = 21,
                    .retransmit_count = 1000,
                },
            },
        .expected_final_state = TwimeSessionState::Active,
    };
}

TwimeCertScenario heartbeat_rate_violation() {
    return {
        .scenario_id = "twime_heartbeat_rate_violation",
        .title = "Synthetic TWIME heartbeat rate violation",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1000),
                {.kind = TwimeCertScenarioActionKind::CommandHeartbeat},
                {.kind = TwimeCertScenarioActionKind::CommandHeartbeat},
                {.kind = TwimeCertScenarioActionKind::CommandHeartbeat},
                {.kind = TwimeCertScenarioActionKind::CommandHeartbeat},
            },
        .expected_final_state = TwimeSessionState::Faulted,
    };
}

TwimeCertScenario flood_reject() {
    return {
        .scenario_id = "twime_flood_reject",
        .title = "Synthetic TWIME flood reject",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1000),
                {
                    .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                    .message = request("FloodReject", 5007,
                                       {
                                           {"ClOrdID", TwimeFieldValue::unsigned_integer(202)},
                                           {"QueueSize", TwimeFieldValue::unsigned_integer(2)},
                                           {"PenaltyRemain", TwimeFieldValue::unsigned_integer(1000)},
                                       }),
                },
            },
        .expected_final_state = TwimeSessionState::Active,
    };
}

TwimeCertScenario business_reject() {
    return {
        .scenario_id = "twime_business_reject",
        .title = "Synthetic TWIME business reject",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1000),
                {
                    .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                    .message = request("BusinessMessageReject", 5009,
                                       {
                                           {"ClOrdID", TwimeFieldValue::unsigned_integer(202)},
                                           {"Timestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'300ULL)},
                                           {"OrdRejReason", TwimeFieldValue::signed_integer(-12)},
                                       }),
                },
            },
        .expected_final_state = TwimeSessionState::Active,
    };
}

TwimeCertScenario business_reject_non_recoverable() {
    return {
        .scenario_id = "twime_business_reject_non_recoverable",
        .title = "Synthetic TWIME business reject non-recoverable",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1000),
                {
                    .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                    .message = request("BusinessMessageReject", 5009,
                                       {
                                           {"ClOrdID", TwimeFieldValue::unsigned_integer(202)},
                                           {"Timestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'300ULL)},
                                           {"OrdRejReason", TwimeFieldValue::signed_integer(-12)},
                                       }),
                },
            },
        .expected_final_state = TwimeSessionState::Active,
    };
}

} // namespace

TwimeCertScenarioResult TwimeCertScenarioRunner::run(const TwimeCertScenario& scenario) const {
    TwimeFakeClock clock(1'715'000'000);
    TwimeFakeTransport transport;
    TwimeInMemoryRecoveryStateStore recovery_store;
    TwimeSession session(scenario.config, transport, recovery_store, clock);
    moex::twime_sbe::TwimeCodec codec;

    TwimeCertScenarioResult result{
        .scenario_id = scenario.scenario_id,
        .title = scenario.title,
    };

    for (const auto& action : scenario.actions) {
        switch (action.kind) {
        case TwimeCertScenarioActionKind::Connect:
            session.apply_command({TwimeSessionCommandType::ConnectFake});
            break;
        case TwimeCertScenarioActionKind::AdvanceClock:
            clock.advance(action.advance_ms);
            break;
        case TwimeCertScenarioActionKind::TimerTick:
            session.on_timer_tick();
            break;
        case TwimeCertScenarioActionKind::InjectInboundMessage: {
            std::vector<std::byte> bytes;
            const auto encode_error = codec.encode_message(action.message, bytes);
            if (encode_error != moex::twime_sbe::TwimeDecodeError::Ok) {
                result.error_message = "failed to encode scripted inbound TWIME message";
                result.final_state = session.state();
                result.cert_log_lines = session.cert_log_lines();
                return result;
            }
            transport.script_inbound_frame(TwimeFakeTransportFrame{
                .bytes = bytes,
                .sequence_number = action.sequence_number,
                .consumes_sequence = action.consumes_sequence,
            });
            session.poll_transport();
            break;
        }
        case TwimeCertScenarioActionKind::InjectPeerClose:
            transport.script_peer_close();
            session.poll_transport();
            break;
        case TwimeCertScenarioActionKind::CommandHeartbeat:
            session.apply_command({TwimeSessionCommandType::SendHeartbeat});
            break;
        case TwimeCertScenarioActionKind::CommandTerminate:
            session.apply_command({TwimeSessionCommandType::SendTerminate});
            break;
        case TwimeCertScenarioActionKind::CommandRetransmit:
            session.apply_command({
                .type = TwimeSessionCommandType::RequestRetransmit,
                .from_seq_no = action.retransmit_from_seq_no,
                .count = action.retransmit_count,
            });
            break;
        }

        auto events = session.drain_events();
        result.events.insert(result.events.end(), events.begin(), events.end());
    }

    result.final_state = session.state();
    result.cert_log_lines = session.cert_log_lines();
    if (scenario.expected_final_state.has_value() && result.final_state != *scenario.expected_final_state) {
        result.error_message = "unexpected final TWIME fake-session state";
    }
    return result;
}

std::optional<TwimeCertScenario> TwimeCertScenarioRunner::builtin(std::string_view scenario_id) {
    if (scenario_id == "twime_session_establish") {
        return session_establish();
    }
    if (scenario_id == "twime_session_establish_ack_sets_inbound_counter") {
        return session_establish_ack_sets_inbound_counter();
    }
    if (scenario_id == "twime_session_reject") {
        return session_reject();
    }
    if (scenario_id == "twime_heartbeat_sequence") {
        return heartbeat_sequence();
    }
    if (scenario_id == "twime_client_sequence_heartbeat_null_nextseqno") {
        return client_sequence_heartbeat_null_nextseqno();
    }
    if (scenario_id == "twime_terminate") {
        return terminate();
    }
    if (scenario_id == "twime_terminate_requires_inbound_terminate") {
        return terminate_requires_inbound_terminate();
    }
    if (scenario_id == "twime_retransmit_last5") {
        return retransmit_last5();
    }
    if (scenario_id == "twime_normal_retransmit_limit_10") {
        return normal_retransmit_limit_10();
    }
    if (scenario_id == "twime_full_recovery_retransmit_limit_1000") {
        return full_recovery_retransmit_limit_1000();
    }
    if (scenario_id == "twime_heartbeat_rate_violation") {
        return heartbeat_rate_violation();
    }
    if (scenario_id == "twime_flood_reject") {
        return flood_reject();
    }
    if (scenario_id == "twime_business_reject") {
        return business_reject();
    }
    if (scenario_id == "twime_business_reject_non_recoverable") {
        return business_reject_non_recoverable();
    }
    return std::nullopt;
}

} // namespace moex::twime_trade
