#include "moex/twime_trade/twime_cert_scenario_runner.hpp"

#include <optional>

namespace moex::twime_trade {

namespace {

using moex::twime_sbe::TwimeEncodeRequest;
using moex::twime_sbe::TwimeFieldInput;
using moex::twime_sbe::TwimeFieldValue;

TwimeEncodeRequest request(std::string_view name, std::uint16_t template_id, std::initializer_list<TwimeFieldInput> fields) {
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
    config.heartbeat_interval_ms = 1000;
    return config;
}

TwimeCertScenario session_establish() {
    return TwimeCertScenario{
        .scenario_id = "twime_session_establish",
        .title = "Synthetic TWIME session establish",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                {.kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                 .message = request("EstablishmentAck", 5001,
                                    {
                                        {"RequestTimestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'100ULL)},
                                        {"KeepaliveInterval", TwimeFieldValue::delta_millisecs(1000)},
                                        {"NextSeqNo", TwimeFieldValue::unsigned_integer(11)},
                                    })},
            },
    };
}

TwimeCertScenario session_reject() {
    return TwimeCertScenario{
        .scenario_id = "twime_session_reject",
        .title = "Synthetic TWIME session reject",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                {.kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                 .message = request("EstablishmentReject", 5002,
                                    {
                                        {"RequestTimestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'100ULL)},
                                        {"EstablishmentRejectCode", TwimeFieldValue::enum_name("Credentials")},
                                    })},
            },
    };
}

TwimeCertScenario heartbeat_sequence() {
    return TwimeCertScenario{
        .scenario_id = "twime_heartbeat_sequence",
        .title = "Synthetic TWIME heartbeat sequence",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                {.kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                 .message = request("EstablishmentAck", 5001,
                                    {
                                        {"RequestTimestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'100ULL)},
                                        {"KeepaliveInterval", TwimeFieldValue::delta_millisecs(1000)},
                                        {"NextSeqNo", TwimeFieldValue::unsigned_integer(11)},
                                    })},
                {.kind = TwimeCertScenarioActionKind::AdvanceClock, .advance_ms = 1000},
                {.kind = TwimeCertScenarioActionKind::TimerTick},
                {.kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                 .message = request("Sequence", 5006, {{"NextSeqNo", TwimeFieldValue::unsigned_integer(1)}})},
            },
    };
}

TwimeCertScenario terminate() {
    return TwimeCertScenario{
        .scenario_id = "twime_terminate",
        .title = "Synthetic TWIME terminate",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                {.kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                 .message = request("EstablishmentAck", 5001,
                                    {
                                        {"RequestTimestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'100ULL)},
                                        {"KeepaliveInterval", TwimeFieldValue::delta_millisecs(1000)},
                                        {"NextSeqNo", TwimeFieldValue::unsigned_integer(11)},
                                    })},
                {.kind = TwimeCertScenarioActionKind::CommandTerminate},
                {.kind = TwimeCertScenarioActionKind::InjectPeerClose},
            },
    };
}

TwimeCertScenario retransmit_last5() {
    return TwimeCertScenario{
        .scenario_id = "twime_retransmit_last5",
        .title = "Synthetic TWIME retransmit last 5",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                {.kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                 .message = request("EstablishmentAck", 5001,
                                    {
                                        {"RequestTimestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'100ULL)},
                                        {"KeepaliveInterval", TwimeFieldValue::delta_millisecs(1000)},
                                        {"NextSeqNo", TwimeFieldValue::unsigned_integer(11)},
                                    })},
                {.kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                 .message = request("Sequence", 5006, {{"NextSeqNo", TwimeFieldValue::unsigned_integer(6)}})},
                {.kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                 .message = request("Retransmission", 5005,
                                    {
                                        {"NextSeqNo", TwimeFieldValue::unsigned_integer(6)},
                                        {"RequestTimestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'200ULL)},
                                        {"Count", TwimeFieldValue::unsigned_integer(5)},
                                    })},
            },
    };
}

TwimeCertScenario flood_reject() {
    return TwimeCertScenario{
        .scenario_id = "twime_flood_reject",
        .title = "Synthetic TWIME flood reject",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                {.kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                 .message = request("EstablishmentAck", 5001,
                                    {
                                        {"RequestTimestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'100ULL)},
                                        {"KeepaliveInterval", TwimeFieldValue::delta_millisecs(1000)},
                                        {"NextSeqNo", TwimeFieldValue::unsigned_integer(11)},
                                    })},
                {.kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                 .message = request("FloodReject", 5007,
                                    {
                                        {"ClOrdID", TwimeFieldValue::unsigned_integer(202)},
                                        {"QueueSize", TwimeFieldValue::unsigned_integer(2)},
                                        {"PenaltyRemain", TwimeFieldValue::unsigned_integer(1000)},
                                    })},
            },
    };
}

TwimeCertScenario business_reject() {
    return TwimeCertScenario{
        .scenario_id = "twime_business_reject",
        .title = "Synthetic TWIME business reject",
        .config = default_config(),
        .actions =
            {
                {.kind = TwimeCertScenarioActionKind::Connect},
                {.kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                 .message = request("EstablishmentAck", 5001,
                                    {
                                        {"RequestTimestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'100ULL)},
                                        {"KeepaliveInterval", TwimeFieldValue::delta_millisecs(1000)},
                                        {"NextSeqNo", TwimeFieldValue::unsigned_integer(11)},
                                    })},
                {.kind = TwimeCertScenarioActionKind::InjectInboundMessage,
                 .message = request("BusinessMessageReject", 5009,
                                    {
                                        {"ClOrdID", TwimeFieldValue::unsigned_integer(202)},
                                        {"Timestamp", TwimeFieldValue::timestamp(1'715'000'000'000'000'300ULL)},
                                        {"OrdRejReason", TwimeFieldValue::signed_integer(-12)},
                                    })},
            },
    };
}

}  // namespace

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
                break;
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
    return result;
}

std::optional<TwimeCertScenario> TwimeCertScenarioRunner::builtin(std::string_view scenario_id) {
    if (scenario_id == "twime_session_establish") {
        return session_establish();
    }
    if (scenario_id == "twime_session_reject") {
        return session_reject();
    }
    if (scenario_id == "twime_heartbeat_sequence") {
        return heartbeat_sequence();
    }
    if (scenario_id == "twime_terminate") {
        return terminate();
    }
    if (scenario_id == "twime_retransmit_last5") {
        return retransmit_last5();
    }
    if (scenario_id == "twime_flood_reject") {
        return flood_reject();
    }
    if (scenario_id == "twime_business_reject") {
        return business_reject();
    }
    return std::nullopt;
}

}  // namespace moex::twime_trade
