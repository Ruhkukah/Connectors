#include "moex/twime_trade/twime_cert_scenario_runner.hpp"

#include "moex/twime_sbe/twime_schema.hpp"

namespace moex::twime_trade {

namespace {

using moex::twime_sbe::TwimeEncodeRequest;
using moex::twime_sbe::TwimeFieldInput;
using moex::twime_sbe::TwimeFieldKind;
using moex::twime_sbe::TwimeFieldMetadata;
using moex::twime_sbe::TwimeFieldValue;
using moex::twime_sbe::TwimePrimitiveType;
using moex::twime_sbe::TwimeSchemaView;

TwimeEncodeRequest request(std::string_view name, std::uint16_t template_id,
                           std::initializer_list<TwimeFieldInput> fields = {}) {
    TwimeEncodeRequest out;
    out.message_name = std::string(name);
    out.template_id = template_id;
    out.fields.assign(fields.begin(), fields.end());
    return out;
}

TwimeFieldValue sample_value_for_field(const TwimeFieldMetadata& field) {
    if (field.name == "ClOrdID") {
        return TwimeFieldValue::unsigned_integer(102);
    }
    if (field.name == "ExpireDate") {
        return TwimeFieldValue::timestamp(moex::twime_sbe::kTwimeTimestampNull);
    }
    if (field.name == "Timestamp" || field.name == "RequestTimestamp") {
        return TwimeFieldValue::timestamp(1'715'000'000'000'000'000ULL);
    }
    if (field.name == "KeepaliveInterval") {
        return TwimeFieldValue::delta_millisecs(1000);
    }
    if (field.name == "Credentials") {
        return TwimeFieldValue::string("LOGIN");
    }
    if (field.name == "Price" || field.name == "LastPx" || field.name == "LegPrice") {
        return TwimeFieldValue::decimal(100000);
    }
    if (field.name == "SecurityID") {
        return TwimeFieldValue::signed_integer(347990);
    }
    if (field.name == "ClOrdLinkID") {
        return TwimeFieldValue::signed_integer(7895424);
    }
    if (field.name == "OrderQty") {
        return TwimeFieldValue::unsigned_integer(5);
    }
    if (field.name == "ComplianceID") {
        return TwimeFieldValue::enum_name("Algorithm");
    }
    if (field.name == "TimeInForce") {
        return TwimeFieldValue::enum_name("Day");
    }
    if (field.name == "Side") {
        return TwimeFieldValue::enum_name("Buy");
    }
    if (field.name == "ClientFlags" || field.name == "Flags" || field.name == "Flags2") {
        return TwimeFieldValue::unsigned_integer(0);
    }
    if (field.name == "Account") {
        return TwimeFieldValue::string("AAAA");
    }
    if (field.name == "OrderID") {
        return TwimeFieldValue::signed_integer(9001001);
    }
    if (field.name == "TradingSessionID") {
        return TwimeFieldValue::signed_integer(1200);
    }
    if (field.name == "FromSeqNo") {
        return TwimeFieldValue::unsigned_integer(21);
    }
    if (field.name == "NextSeqNo") {
        return TwimeFieldValue::unsigned_integer(26);
    }
    if (field.name == "Count") {
        return TwimeFieldValue::unsigned_integer(5);
    }
    if (field.name == "TerminationCode") {
        return TwimeFieldValue::enum_name("Finished");
    }
    if (field.name == "EstablishmentRejectCode") {
        return TwimeFieldValue::enum_name("Credentials");
    }
    if (field.name == "QueueSize") {
        return TwimeFieldValue::unsigned_integer(2);
    }
    if (field.name == "PenaltyRemain") {
        return TwimeFieldValue::unsigned_integer(1000);
    }
    if (field.name == "SessionRejectReason") {
        return TwimeFieldValue::enum_name("Other");
    }
    if (field.name == "OrdRejReason") {
        return TwimeFieldValue::signed_integer(-12);
    }

    const auto& type = *field.type;
    switch (type.kind) {
    case TwimeFieldKind::Primitive:
        if (type.primitive_type == TwimePrimitiveType::Int8 || type.primitive_type == TwimePrimitiveType::Int16 ||
            type.primitive_type == TwimePrimitiveType::Int32 || type.primitive_type == TwimePrimitiveType::Int64) {
            return TwimeFieldValue::signed_integer(1);
        }
        return TwimeFieldValue::unsigned_integer(1);
    case TwimeFieldKind::String:
        return TwimeFieldValue::string("X");
    case TwimeFieldKind::TimeStamp:
        return TwimeFieldValue::timestamp(1'715'000'000'000'000'000ULL);
    case TwimeFieldKind::DeltaMillisecs:
        return TwimeFieldValue::delta_millisecs(1000);
    case TwimeFieldKind::Decimal5:
        return TwimeFieldValue::decimal(100000);
    case TwimeFieldKind::Enum:
        return TwimeFieldValue::enum_name(type.enum_metadata->values[0].name);
    case TwimeFieldKind::Set:
        return TwimeFieldValue::set_name(type.set_metadata->choices[0].name);
    case TwimeFieldKind::Composite:
    default:
        return TwimeFieldValue::unsigned_integer(0);
    }
}

TwimeEncodeRequest request_with_defaults(std::string_view name, std::uint16_t template_id) {
    const auto* metadata = TwimeSchemaView::find_message_by_name(name);
    if (metadata == nullptr) {
        return request(name, template_id);
    }

    std::vector<TwimeFieldInput> fields;
    fields.reserve(metadata->field_count);
    for (std::size_t index = 0; index < metadata->field_count; ++index) {
        const auto& field = metadata->fields[index];
        fields.push_back({std::string(field.name), sample_value_for_field(field)});
    }

    TwimeEncodeRequest out;
    out.message_name = std::string(name);
    out.template_id = template_id;
    out.fields = std::move(fields);
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
                {
                    .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                    .message = request_with_defaults("NewOrderSingleResponse", 7015),
                    .sequence_number = 11,
                },
                {
                    .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                    .message = request_with_defaults("NewOrderSingleResponse", 7015),
                    .sequence_number = 12,
                },
                {
                    .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                    .message = request_with_defaults("NewOrderSingleResponse", 7015),
                    .sequence_number = 13,
                },
                {
                    .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                    .message = request_with_defaults("NewOrderSingleResponse", 7015),
                    .sequence_number = 14,
                },
                {
                    .kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                    .message = request_with_defaults("NewOrderSingleResponse", 7015),
                    .sequence_number = 15,
                },
            },
        .expected_final_state = TwimeSessionState::Active,
    };
}

TwimeCertScenario message_counter_reset() {
    auto config = default_config();
    config.session_id = "twime_phase2b_message_counter_reset";
    return {
        .scenario_id = "twime_message_counter_reset",
        .title = "Synthetic TWIME message counter reset",
        .config = config,
        .initial_recovery_state =
            TwimeRecoveryState{
                .next_outbound_seq = 12,
                .next_expected_inbound_seq = 21,
                .last_establishment_id = 1'715'000'000'000'000'003ULL,
                .recovery_epoch = 5,
                .last_clean_shutdown = false,
            },
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                establish_ack_action(11, 1000),
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
    if (scenario.initial_recovery_state.has_value()) {
        recovery_store.save(scenario.config.session_id, *scenario.initial_recovery_state);
    }
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
    if (scenario_id == "twime_message_counter_reset") {
        return message_counter_reset();
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
