#include "moex/twime_trade/twime_session.hpp"

#include "moex/twime_sbe/twime_schema.hpp"

#include <sstream>

namespace moex::twime_trade {

namespace {

using moex::twime_sbe::DecodedTwimeField;
using moex::twime_sbe::DecodedTwimeMessage;
using moex::twime_sbe::TwimeDecodeError;
using moex::twime_sbe::TwimeEncodeRequest;
using moex::twime_sbe::TwimeFieldInput;
using moex::twime_sbe::TwimeFieldKind;
using moex::twime_sbe::TwimeFieldValue;
using moex::twime_sbe::kTwimeTimestampNull;

constexpr std::uint16_t kTemplateEstablish = 5000;
constexpr std::uint16_t kTemplateEstablishmentAck = 5001;
constexpr std::uint16_t kTemplateEstablishmentReject = 5002;
constexpr std::uint16_t kTemplateTerminate = 5003;
constexpr std::uint16_t kTemplateRetransmitRequest = 5004;
constexpr std::uint16_t kTemplateRetransmission = 5005;
constexpr std::uint16_t kTemplateSequence = 5006;
constexpr std::uint16_t kTemplateFloodReject = 5007;
constexpr std::uint16_t kTemplateSessionReject = 5008;
constexpr std::uint16_t kTemplateBusinessReject = 5009;

const DecodedTwimeField* find_field(const DecodedTwimeMessage& message, std::string_view name) {
    for (const auto& field : message.fields) {
        if (field.metadata != nullptr && field.metadata->name == name) {
            return &field;
        }
    }
    return nullptr;
}

std::optional<std::uint64_t> field_unsigned(const DecodedTwimeMessage& message, std::string_view name) {
    const auto* field = find_field(message, name);
    if (field == nullptr) {
        return std::nullopt;
    }
    const auto& type = *field->metadata->type;
    if (type.kind == TwimeFieldKind::TimeStamp && field->value.unsigned_value == kTwimeTimestampNull) {
        return std::nullopt;
    }
    if (field->metadata->nullable && field->metadata->has_null_value &&
        field->value.unsigned_value == field->metadata->null_value) {
        return std::nullopt;
    }
    return field->value.unsigned_value;
}

std::optional<std::int64_t> field_signed(const DecodedTwimeMessage& message, std::string_view name) {
    const auto* field = find_field(message, name);
    if (field == nullptr) {
        return std::nullopt;
    }
    const auto& type = *field->metadata->type;
    if (type.kind == TwimeFieldKind::Primitive &&
        (type.primitive_type == moex::twime_sbe::TwimePrimitiveType::Int8 ||
         type.primitive_type == moex::twime_sbe::TwimePrimitiveType::Int16 ||
         type.primitive_type == moex::twime_sbe::TwimePrimitiveType::Int32 ||
         type.primitive_type == moex::twime_sbe::TwimePrimitiveType::Int64)) {
        return field->value.signed_value;
    }
    return static_cast<std::int64_t>(field->value.unsigned_value);
}

std::string state_name(TwimeSessionState state) {
    switch (state) {
    case TwimeSessionState::Created:
        return "Created";
    case TwimeSessionState::ConnectingFake:
        return "ConnectingFake";
    case TwimeSessionState::Establishing:
        return "Establishing";
    case TwimeSessionState::Active:
        return "Active";
    case TwimeSessionState::Terminating:
        return "Terminating";
    case TwimeSessionState::Terminated:
        return "Terminated";
    case TwimeSessionState::Rejected:
        return "Rejected";
    case TwimeSessionState::Faulted:
        return "Faulted";
    case TwimeSessionState::Recovering:
        return "Recovering";
    }
    return "Unknown";
}

TwimeEncodeRequest build_request(std::string_view message_name, std::uint16_t template_id,
                                 std::initializer_list<TwimeFieldInput> fields) {
    TwimeEncodeRequest request;
    request.message_name = std::string(message_name);
    request.template_id = template_id;
    request.fields.assign(fields.begin(), fields.end());
    return request;
}

bool template_counts_toward_sequence(std::uint16_t template_id) {
    return template_id >= 6000 || template_id >= 7000;
}

}  // namespace

TwimeSession::TwimeSession(TwimeSessionConfig config, TwimeFakeTransport& transport,
                           TwimeRecoveryStateStore& recovery_store, TwimeFakeClock& clock)
    : config_(std::move(config)),
      transport_(transport),
      recovery_store_(recovery_store),
      clock_(clock),
      outbound_journal_(config_.journal_capacity),
      inbound_journal_(config_.journal_capacity),
      rate_limit_model_(config_.max_messages_per_window, config_.rate_limit_window_ms) {
    recovery_state_.recovery_epoch = config_.initial_recovery_epoch;
}

TwimeSessionState TwimeSession::state() const noexcept {
    return state_;
}

std::optional<std::int64_t> TwimeSession::last_reject_code() const noexcept {
    return last_reject_code_;
}

const TwimeSequenceState& TwimeSession::sequence_state() const noexcept {
    return sequence_state_;
}

const TwimeRecoveryState& TwimeSession::recovery_state() const noexcept {
    return recovery_state_;
}

const TwimeOutboundJournal& TwimeSession::outbound_journal() const noexcept {
    return outbound_journal_;
}

const TwimeInboundJournal& TwimeSession::inbound_journal() const noexcept {
    return inbound_journal_;
}

const std::vector<std::string>& TwimeSession::cert_log_lines() const noexcept {
    return cert_log_lines_;
}

void TwimeSession::apply_command(const TwimeSessionCommand& command) {
    switch (command.type) {
    case TwimeSessionCommandType::ConnectFake:
        connect_fake();
        break;
    case TwimeSessionCommandType::SendTerminate:
        send_terminate();
        break;
    case TwimeSessionCommandType::SendHeartbeat:
        send_heartbeat();
        break;
    case TwimeSessionCommandType::RequestRetransmit:
        send_retransmit_request(command.from_seq_no, command.count);
        break;
    case TwimeSessionCommandType::PollTransport:
        poll_transport();
        break;
    }
}

void TwimeSession::on_timer_tick() {
    if (state_ != TwimeSessionState::Active) {
        return;
    }
    if (clock_.now_ms() < heartbeat_due_ms_) {
        return;
    }
    send_heartbeat();
}

void TwimeSession::poll_transport() {
    for (const auto& event : transport_.drain_inbound()) {
        process_transport_event(event);
    }
}

std::vector<TwimeSessionEvent> TwimeSession::drain_events() {
    std::vector<TwimeSessionEvent> events = pending_events_;
    pending_events_.clear();
    return events;
}

void TwimeSession::connect_fake() {
    if (state_ != TwimeSessionState::Created && state_ != TwimeSessionState::Terminated &&
        state_ != TwimeSessionState::Rejected) {
        return;
    }

    if (const auto snapshot = recovery_store_.load(config_.session_id)) {
        recovery_state_ = *snapshot;
        sequence_state_.reset(recovery_state_.next_outbound_seq, recovery_state_.next_expected_inbound_seq);
        if (!recovery_state_.last_clean_shutdown) {
            ++recovery_state_.recovery_epoch;
            transition_to(TwimeSessionState::Recovering, "dirty shutdown snapshot loaded");
        }
    } else {
        recovery_state_ = TwimeRecoveryState{
            .next_outbound_seq = 1,
            .next_expected_inbound_seq = 1,
            .last_establishment_id = 0,
            .recovery_epoch = config_.initial_recovery_epoch,
            .last_clean_shutdown = true,
        };
        sequence_state_.reset();
    }

    transport_.connect();
    transition_to(TwimeSessionState::ConnectingFake, "fake transport connected");

    recovery_state_.last_clean_shutdown = false;
    recovery_state_.last_establishment_id = clock_.now_ns();
    persist_recovery_state();

    transition_to(TwimeSessionState::Establishing, "emitting Establish");
    send_message(
        build_request("Establish", kTemplateEstablish,
                      {
                          {"Timestamp", TwimeFieldValue::timestamp(recovery_state_.last_establishment_id)},
                          {"KeepaliveInterval", TwimeFieldValue::delta_millisecs(config_.keepalive_interval_ms)},
                          {"Credentials", TwimeFieldValue::string(config_.credentials)},
                      }),
        false, TwimeSessionEventType::OutboundMessage, "Establish sent");
}

void TwimeSession::send_terminate() {
    if (state_ != TwimeSessionState::Active && state_ != TwimeSessionState::Recovering &&
        state_ != TwimeSessionState::Establishing) {
        return;
    }
    send_message(build_request("Terminate", kTemplateTerminate, {{"TerminationCode", TwimeFieldValue::enum_name("Finished")}}),
                 false, TwimeSessionEventType::OutboundMessage, "Terminate sent");
    transition_to(TwimeSessionState::Terminating, "awaiting fake peer close");
}

void TwimeSession::send_heartbeat() {
    if (state_ != TwimeSessionState::Active) {
        return;
    }
    if (!send_message(build_request("Sequence", kTemplateSequence,
                                    {{"NextSeqNo", TwimeFieldValue::unsigned_integer(sequence_state_.next_outbound_seq())}}),
                      false, TwimeSessionEventType::HeartbeatSent, "Sequence heartbeat sent")) {
        return;
    }
    heartbeat_due_ms_ = clock_.now_ms() + config_.heartbeat_interval_ms;
}

void TwimeSession::send_retransmit_request(std::uint64_t from_seq_no, std::uint32_t count) {
    if (count == 0) {
        return;
    }
    send_message(build_request("RetransmitRequest", kTemplateRetransmitRequest,
                               {
                                   {"Timestamp", TwimeFieldValue::timestamp(clock_.now_ns())},
                                   {"FromSeqNo", TwimeFieldValue::unsigned_integer(from_seq_no)},
                                   {"Count", TwimeFieldValue::unsigned_integer(count)},
                               }),
                 false, TwimeSessionEventType::RetransmitRequested, "RetransmitRequest sent");
}

void TwimeSession::process_transport_event(const TwimeFakeTransportEvent& event) {
    if (event.kind == TwimeFakeTransportEventKind::PeerClosed) {
        transport_.disconnect();
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::PeerClosed,
            .state = state_,
            .summary = "fake peer closed transport",
        });
        if (state_ == TwimeSessionState::Terminating) {
            recovery_state_.last_clean_shutdown = true;
            persist_recovery_state();
            transition_to(TwimeSessionState::Terminated, "fake peer close after Terminate");
        } else {
            transition_to(TwimeSessionState::Faulted, "unexpected peer close");
        }
        return;
    }
    process_inbound_frame(event.frame);
}

void TwimeSession::process_inbound_frame(const TwimeFakeTransportFrame& frame) {
    if (frame.bytes.size() > config_.max_frame_size) {
        transition_to(TwimeSessionState::Faulted, "inbound frame exceeds max_frame_size");
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::Faulted,
            .state = state_,
            .summary = "inbound frame exceeds max_frame_size",
        });
        return;
    }

    DecodedTwimeMessage decoded;
    const auto error = codec_.decode_message(frame.bytes, decoded);
    if (error != TwimeDecodeError::Ok) {
        transition_to(TwimeSessionState::Faulted, "TWIME decode failure");
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::Faulted,
            .state = state_,
            .summary = "decode error",
        });
        return;
    }

    if (frame.consumes_sequence) {
        const auto observation = sequence_state_.observe_inbound(frame.sequence_number, true);
        recovery_state_.next_expected_inbound_seq = sequence_state_.next_expected_inbound_seq();
        persist_recovery_state();
        if (observation.gap.has_value()) {
            transition_to(TwimeSessionState::Recovering, "inbound sequence gap detected");
            append_event(TwimeSessionEvent{
                .type = TwimeSessionEventType::SequenceGapDetected,
                .state = state_,
                .template_id = decoded.header.template_id,
                .sequence_number = frame.sequence_number,
                .gap_from = observation.gap->from_seq_no,
                .gap_to = observation.gap->to_seq_no,
                .message_name = std::string(decoded.metadata->name),
                .summary = "inbound sequence gap detected",
            });
            if (config_.auto_request_retransmit_on_gap) {
                send_retransmit_request(observation.gap->from_seq_no,
                                        static_cast<std::uint32_t>(observation.gap->to_seq_no -
                                                                   observation.gap->from_seq_no + 1));
            }
        }
    }

    const auto formatted = formatter_.format(decoded);
    inbound_journal_.append(TwimeJournalEntry{
        .sequence_number = frame.sequence_number,
        .consumes_sequence = frame.consumes_sequence,
        .template_id = decoded.header.template_id,
        .message_name = std::string(decoded.metadata->name),
        .bytes = frame.bytes,
        .cert_log_line = "IN " + formatted,
    });
    append_cert_log("IN", formatted);
    append_event(TwimeSessionEvent{
        .type = TwimeSessionEventType::InboundMessage,
        .state = state_,
        .template_id = decoded.header.template_id,
        .sequence_number = frame.sequence_number,
        .message_name = std::string(decoded.metadata->name),
        .summary = "inbound message received",
        .cert_log_line = formatted,
    });

    process_inbound_message(decoded, frame);
}

void TwimeSession::process_inbound_message(const DecodedTwimeMessage& message, const TwimeFakeTransportFrame& frame) {
    switch (message.header.template_id) {
    case kTemplateEstablishmentAck: {
        if (const auto next_seq = field_unsigned(message, "NextSeqNo")) {
            sequence_state_.restore_outbound_next(*next_seq);
            recovery_state_.next_outbound_seq = *next_seq;
            persist_recovery_state();
        }
        heartbeat_due_ms_ = clock_.now_ms() + config_.heartbeat_interval_ms;
        transition_to(TwimeSessionState::Active, "EstablishmentAck received");
        break;
    }
    case kTemplateEstablishmentReject: {
        last_reject_code_ = field_unsigned(message, "EstablishmentRejectCode").value_or(0);
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::EstablishmentRejected,
            .state = TwimeSessionState::Rejected,
            .template_id = message.header.template_id,
            .sequence_number = frame.sequence_number,
            .reason_code = *last_reject_code_,
            .message_name = std::string(message.metadata->name),
            .summary = "EstablishmentReject received",
            .cert_log_line = formatter_.format(message),
        });
        transition_to(TwimeSessionState::Rejected, "EstablishmentReject received");
        break;
    }
    case kTemplateSequence: {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::HeartbeatReceived,
            .state = state_,
            .template_id = message.header.template_id,
            .sequence_number = frame.sequence_number,
            .message_name = std::string(message.metadata->name),
            .summary = "Sequence heartbeat received",
            .cert_log_line = formatter_.format(message),
        });
        if (const auto next_seq = field_unsigned(message, "NextSeqNo")) {
            if (const auto gap = sequence_state_.observe_peer_next_seq(*next_seq)) {
                transition_to(TwimeSessionState::Recovering, "peer NextSeqNo indicates a gap");
                append_event(TwimeSessionEvent{
                    .type = TwimeSessionEventType::SequenceGapDetected,
                    .state = state_,
                    .template_id = message.header.template_id,
                    .gap_from = gap->from_seq_no,
                    .gap_to = gap->to_seq_no,
                    .message_name = std::string(message.metadata->name),
                    .summary = "peer NextSeqNo indicates a gap",
                    .cert_log_line = formatter_.format(message),
                });
                if (config_.auto_request_retransmit_on_gap) {
                    send_retransmit_request(gap->from_seq_no,
                                            static_cast<std::uint32_t>(gap->to_seq_no - gap->from_seq_no + 1));
                }
            }
        }
        break;
    }
    case kTemplateRetransmission: {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::RetransmissionReceived,
            .state = state_,
            .template_id = message.header.template_id,
            .sequence_number = frame.sequence_number,
            .message_name = std::string(message.metadata->name),
            .summary = "Retransmission metadata received",
            .cert_log_line = formatter_.format(message),
        });
        if (const auto next_seq = field_unsigned(message, "NextSeqNo")) {
            sequence_state_.restore_expected_inbound_next(*next_seq);
            recovery_state_.next_expected_inbound_seq = *next_seq;
            persist_recovery_state();
        }
        if (state_ == TwimeSessionState::Recovering) {
            transition_to(TwimeSessionState::Active, "Retransmission received");
        }
        break;
    }
    case kTemplateFloodReject: {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::FloodRejectReceived,
            .state = state_,
            .template_id = message.header.template_id,
            .sequence_number = frame.sequence_number,
            .message_name = std::string(message.metadata->name),
            .summary = "FloodReject received",
            .cert_log_line = formatter_.format(message),
        });
        break;
    }
    case kTemplateSessionReject: {
        last_reject_code_ = field_unsigned(message, "SessionRejectReason").value_or(0);
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::SessionRejectReceived,
            .state = state_,
            .template_id = message.header.template_id,
            .sequence_number = frame.sequence_number,
            .reason_code = *last_reject_code_,
            .message_name = std::string(message.metadata->name),
            .summary = "SessionReject received",
            .cert_log_line = formatter_.format(message),
        });
        break;
    }
    case kTemplateBusinessReject: {
        last_reject_code_ = field_signed(message, "OrdRejReason").value_or(0);
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::BusinessRejectReceived,
            .state = state_,
            .template_id = message.header.template_id,
            .sequence_number = frame.sequence_number,
            .reason_code = *last_reject_code_,
            .message_name = std::string(message.metadata->name),
            .summary = "BusinessMessageReject received",
            .cert_log_line = formatter_.format(message),
        });
        break;
    }
    default:
        break;
    }
}

void TwimeSession::transition_to(TwimeSessionState new_state, std::string summary) {
    state_ = new_state;
    append_event(TwimeSessionEvent{
        .type = TwimeSessionEventType::StateChanged,
        .state = new_state,
        .summary = std::move(summary),
    });
}

void TwimeSession::append_event(TwimeSessionEvent event) {
    if (event.state == TwimeSessionState::Created) {
        event.state = state_;
    }
    pending_events_.push_back(std::move(event));
}

void TwimeSession::append_cert_log(const std::string& direction, const std::string& formatted_message) {
    cert_log_lines_.push_back(direction + " " + formatted_message);
}

void TwimeSession::persist_recovery_state() {
    recovery_state_.next_outbound_seq = sequence_state_.next_outbound_seq();
    recovery_state_.next_expected_inbound_seq = sequence_state_.next_expected_inbound_seq();
    recovery_store_.save(config_.session_id, recovery_state_);
}

bool TwimeSession::send_message(const TwimeEncodeRequest& request, bool consumes_sequence, TwimeSessionEventType event_type,
                                std::string summary) {
    const auto rate_limit_decision = rate_limit_model_.observe_send(clock_.now_ms());
    if (!rate_limit_decision.allowed) {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::Faulted,
            .state = state_,
            .summary = "rate-limit model rejected outbound message",
        });
        return false;
    }

    std::vector<std::byte> bytes;
    const auto encode_error = codec_.encode_message(request, bytes);
    if (encode_error != TwimeDecodeError::Ok) {
        transition_to(TwimeSessionState::Faulted, "failed to encode outbound message");
        return false;
    }

    DecodedTwimeMessage decoded;
    if (codec_.decode_message(bytes, decoded) != TwimeDecodeError::Ok) {
        transition_to(TwimeSessionState::Faulted, "failed to decode outbound message");
        return false;
    }

    const auto transport_sequence = sequence_state_.reserve_outbound_sequence(consumes_sequence);
    persist_recovery_state();

    const auto formatted = formatter_.format(decoded);
    transport_.send_outbound_frame(TwimeFakeTransportFrame{
        .bytes = bytes,
        .sequence_number = transport_sequence,
        .consumes_sequence = consumes_sequence,
    });
    outbound_journal_.append(TwimeJournalEntry{
        .sequence_number = transport_sequence,
        .consumes_sequence = consumes_sequence,
        .template_id = decoded.header.template_id,
        .message_name = std::string(decoded.metadata->name),
        .bytes = bytes,
        .cert_log_line = "OUT " + formatted,
    });
    append_cert_log("OUT", formatted);
    append_event(TwimeSessionEvent{
        .type = event_type,
        .state = state_,
        .template_id = decoded.header.template_id,
        .sequence_number = consumes_sequence ? transport_sequence : 0,
        .message_name = std::string(decoded.metadata->name),
        .summary = std::move(summary),
        .cert_log_line = formatted,
    });
    return true;
}

std::uint64_t TwimeSession::next_heartbeat_due_ms() const noexcept {
    return heartbeat_due_ms_;
}

}  // namespace moex::twime_trade
