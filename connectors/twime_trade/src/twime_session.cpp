#include "moex/twime_trade/twime_session.hpp"

#include "moex/twime_sbe/twime_schema.hpp"

#include <algorithm>
#include <limits>

namespace moex::twime_trade {

namespace {

using moex::twime_sbe::DecodedTwimeField;
using moex::twime_sbe::DecodedTwimeMessage;
using moex::twime_sbe::kTwimeTimestampNull;
using moex::twime_sbe::TwimeDecodeError;
using moex::twime_sbe::TwimeDirection;
using moex::twime_sbe::TwimeEncodeRequest;
using moex::twime_sbe::TwimeFieldInput;
using moex::twime_sbe::TwimeFieldKind;
using moex::twime_sbe::TwimeFieldValue;
using moex::twime_sbe::TwimeLayer;

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
    if (type.kind == TwimeFieldKind::Primitive && (type.primitive_type == moex::twime_sbe::TwimePrimitiveType::Int8 ||
                                                   type.primitive_type == moex::twime_sbe::TwimePrimitiveType::Int16 ||
                                                   type.primitive_type == moex::twime_sbe::TwimePrimitiveType::Int32 ||
                                                   type.primitive_type == moex::twime_sbe::TwimePrimitiveType::Int64)) {
        return field->value.signed_value;
    }
    return static_cast<std::int64_t>(field->value.unsigned_value);
}

TwimeEncodeRequest build_request(std::string_view message_name, std::uint16_t template_id,
                                 std::initializer_list<TwimeFieldInput> fields = {}) {
    TwimeEncodeRequest request;
    request.message_name = std::string(message_name);
    request.template_id = template_id;
    request.fields.assign(fields.begin(), fields.end());
    return request;
}

bool try_add_u64(std::uint64_t left, std::uint64_t right, std::uint64_t& out) {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    out = left + right;
    return true;
}

} // namespace

TwimeSession::TwimeSession(TwimeSessionConfig config, TwimeFakeTransport& transport,
                           TwimeRecoveryStateStore& recovery_store, TwimeFakeClock& clock)
    : config_(std::move(config)), fake_transport_(&transport), recovery_store_(recovery_store), clock_(clock),
      frame_assembler_(config_.max_frame_size), outbound_journal_(config_.journal_capacity),
      inbound_journal_(config_.journal_capacity),
      rate_limit_model_(config_.max_total_messages_per_window, config_.max_trading_messages_per_window,
                        config_.max_heartbeats_per_second, config_.rate_limit_window_ms),
      transport_read_buffer_(std::max<std::size_t>(config_.max_frame_size, moex::twime_sbe::kTwimeMessageHeaderSize)) {
    recovery_state_.recovery_epoch = config_.initial_recovery_epoch;
    active_keepalive_interval_ms_ = config_.keepalive_interval_ms;
}

TwimeSession::TwimeSession(TwimeSessionConfig config, transport::ITwimeByteTransport& transport,
                           TwimeRecoveryStateStore& recovery_store, TwimeFakeClock& clock)
    : config_(std::move(config)), byte_transport_(&transport), recovery_store_(recovery_store), clock_(clock),
      frame_assembler_(config_.max_frame_size), outbound_journal_(config_.journal_capacity),
      inbound_journal_(config_.journal_capacity),
      rate_limit_model_(config_.max_total_messages_per_window, config_.max_trading_messages_per_window,
                        config_.max_heartbeats_per_second, config_.rate_limit_window_ms),
      transport_read_buffer_(std::max<std::size_t>(config_.max_frame_size, moex::twime_sbe::kTwimeMessageHeaderSize)) {
    recovery_state_.recovery_epoch = config_.initial_recovery_epoch;
    active_keepalive_interval_ms_ = config_.keepalive_interval_ms;
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

std::uint32_t TwimeSession::active_keepalive_interval_ms() const noexcept {
    return active_keepalive_interval_ms_;
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

void TwimeSession::force_fault(std::string summary) {
    append_event(TwimeSessionEvent{
        .type = TwimeSessionEventType::Faulted,
        .state = state_,
        .summary = summary,
    });
    transition_to(TwimeSessionState::Faulted, std::move(summary));
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
    if (fake_transport_ != nullptr) {
        poll_fake_transport();
        return;
    }
    poll_byte_transport();
}

void TwimeSession::poll_fake_transport() {
    for (const auto& event : fake_transport_->drain_inbound()) {
        process_transport_event(event);
    }
}

void TwimeSession::poll_byte_transport() {
    if (byte_transport_ == nullptr) {
        return;
    }

    if (byte_transport_->state() == transport::TwimeTransportState::Open && !flush_pending_outbound_bytes()) {
        return;
    }

    while (true) {
        const auto result = byte_transport_->poll_read(transport_read_buffer_);
        switch (result.status) {
        case transport::TwimeTransportStatus::WouldBlock:
            if (result.event == transport::TwimeTransportEvent::ReconnectSuppressed) {
                append_event(TwimeSessionEvent{
                    .type = TwimeSessionEventType::ReconnectTooFast,
                    .state = state_,
                    .summary = "byte-stream transport suppressed reconnect attempt before 1000ms",
                });
            }
            return;
        case transport::TwimeTransportStatus::RemoteClosed:
            handle_peer_close("byte-stream transport remote close");
            return;
        case transport::TwimeTransportStatus::Ok:
            if (result.event == transport::TwimeTransportEvent::OpenSucceeded && result.bytes_transferred == 0) {
                on_byte_transport_opened("byte-stream transport connected");
                if (state_ == TwimeSessionState::Faulted || !flush_pending_outbound_bytes()) {
                    return;
                }
                continue;
            }
            if (result.bytes_transferred == 0) {
                append_event(TwimeSessionEvent{
                    .type = TwimeSessionEventType::Faulted,
                    .state = state_,
                    .summary = "byte-stream transport returned zero-byte successful read",
                });
                transition_to(TwimeSessionState::Faulted, "byte-stream transport returned zero-byte read");
                frame_assembler_.reset();
                return;
            }
            break;
        case transport::TwimeTransportStatus::Closed:
            return;
        case transport::TwimeTransportStatus::Fault:
        case transport::TwimeTransportStatus::InvalidState:
        case transport::TwimeTransportStatus::BufferTooSmall:
            append_event(TwimeSessionEvent{
                .type = TwimeSessionEventType::Faulted,
                .state = state_,
                .summary = "byte-stream transport read fault",
            });
            transition_to(TwimeSessionState::Faulted, "byte-stream transport read fault");
            frame_assembler_.reset();
            return;
        }

        const auto feed_result =
            frame_assembler_.feed(std::span<const std::byte>(transport_read_buffer_.data(), result.bytes_transferred));
        if (feed_result.error != TwimeDecodeError::Ok && feed_result.error != TwimeDecodeError::NeedMoreData) {
            append_event(TwimeSessionEvent{
                .type = TwimeSessionEventType::Faulted,
                .state = state_,
                .summary = "TWIME frame assembler rejected inbound byte stream",
            });
            transition_to(TwimeSessionState::Faulted, "TWIME frame assembler rejected inbound byte stream");
            frame_assembler_.reset();
            return;
        }

        while (frame_assembler_.has_frame()) {
            auto frame = frame_assembler_.pop_frame();
            process_inbound_frame(TwimeFakeTransportFrame{.bytes = std::move(frame.bytes)});
            if (state_ == TwimeSessionState::Faulted) {
                frame_assembler_.reset();
                return;
            }
        }
    }
}

void TwimeSession::on_byte_transport_opened(std::string summary) {
    transition_to(TwimeSessionState::ConnectingFake, std::move(summary));
    transition_to(TwimeSessionState::Establishing, "emitting Establish");
    send_message(
        build_request("Establish", kTemplateEstablish,
                      {
                          {"Timestamp", TwimeFieldValue::timestamp(recovery_state_.last_establishment_id)},
                          {"KeepaliveInterval", TwimeFieldValue::delta_millisecs(config_.keepalive_interval_ms)},
                          {"Credentials", TwimeFieldValue::string(config_.credentials)},
                      }),
        TwimeSessionEventType::OutboundMessage, "Establish sent");
}

std::vector<TwimeSessionEvent> TwimeSession::drain_events() {
    std::vector<TwimeSessionEvent> events = pending_events_;
    pending_events_.clear();
    return events;
}

void TwimeSession::connect_fake() {
    if (is_reconnect_too_fast()) {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::ReconnectTooFast,
            .state = state_,
            .summary = "ConnectFake attempted less than 1000ms after previous Establish attempt",
        });
        return;
    }

    if (state_ != TwimeSessionState::Created && state_ != TwimeSessionState::Terminated &&
        state_ != TwimeSessionState::Rejected) {
        return;
    }

    mark_connect_attempt();
    clear_pending_retransmission();

    if (const auto snapshot = recovery_store_.load(config_.session_id)) {
        recovery_state_ = *snapshot;
        sequence_state_.reset(recovery_state_.next_outbound_seq, recovery_state_.next_expected_inbound_seq);
        recovering_from_dirty_snapshot_ = !recovery_state_.last_clean_shutdown;
        if (recovering_from_dirty_snapshot_) {
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
        recovering_from_dirty_snapshot_ = false;
    }

    if (fake_transport_ != nullptr) {
        fake_transport_->connect();
        transition_to(TwimeSessionState::ConnectingFake, "fake transport connected");
    } else if (byte_transport_ != nullptr) {
        const auto open_result = byte_transport_->open();
        if (open_result.status == transport::TwimeTransportStatus::Fault ||
            open_result.status == transport::TwimeTransportStatus::InvalidState ||
            open_result.status == transport::TwimeTransportStatus::BufferTooSmall ||
            open_result.status == transport::TwimeTransportStatus::Closed ||
            open_result.status == transport::TwimeTransportStatus::RemoteClosed) {
            append_event(TwimeSessionEvent{
                .type = TwimeSessionEventType::Faulted,
                .state = state_,
                .summary = "byte-stream transport open failed",
            });
            transition_to(TwimeSessionState::Faulted, "byte-stream transport open failed");
            return;
        }
        if (open_result.event == transport::TwimeTransportEvent::ReconnectSuppressed) {
            append_event(TwimeSessionEvent{
                .type = TwimeSessionEventType::ReconnectTooFast,
                .state = state_,
                .summary = "byte-stream transport suppressed reconnect attempt before 1000ms",
            });
            return;
        }
        transition_to(TwimeSessionState::ConnectingFake, "byte-stream transport opening");
    }

    active_keepalive_interval_ms_ = config_.keepalive_interval_ms;
    recovery_state_.last_clean_shutdown = false;
    recovery_state_.last_establishment_id = clock_.now_ns();
    terminate_response_received_ = false;
    persist_recovery_state();

    if (fake_transport_ != nullptr) {
        transition_to(TwimeSessionState::Establishing, "emitting Establish");
        send_message(
            build_request("Establish", kTemplateEstablish,
                          {
                              {"Timestamp", TwimeFieldValue::timestamp(recovery_state_.last_establishment_id)},
                              {"KeepaliveInterval", TwimeFieldValue::delta_millisecs(config_.keepalive_interval_ms)},
                              {"Credentials", TwimeFieldValue::string(config_.credentials)},
                          }),
            TwimeSessionEventType::OutboundMessage, "Establish sent");
    } else if (byte_transport_ != nullptr && byte_transport_->state() == transport::TwimeTransportState::Open) {
        on_byte_transport_opened("byte-stream transport connected");
    }
}

void TwimeSession::send_terminate() {
    if (state_ != TwimeSessionState::Active && state_ != TwimeSessionState::Recovering &&
        state_ != TwimeSessionState::Establishing) {
        return;
    }
    if (!send_message(build_request("Terminate", kTemplateTerminate,
                                    {{"TerminationCode", TwimeFieldValue::enum_name("Finished")}}),
                      TwimeSessionEventType::OutboundMessage, "Terminate sent")) {
        return;
    }
    terminate_response_received_ = false;
    transition_to(TwimeSessionState::Terminating, "awaiting inbound Terminate(Finished)");
}

void TwimeSession::send_heartbeat() {
    if (state_ != TwimeSessionState::Active) {
        return;
    }
    if (!send_message(build_request("Sequence", kTemplateSequence), TwimeSessionEventType::HeartbeatSent,
                      "Sequence heartbeat sent")) {
        return;
    }
    heartbeat_due_ms_ = clock_.now_ms() + active_keepalive_interval_ms_;
}

void TwimeSession::send_retransmit_request(std::uint64_t from_seq_no, std::uint32_t count) {
    if (count == 0) {
        return;
    }

    std::uint64_t gap_to = 0;
    if (!try_add_u64(from_seq_no, static_cast<std::uint64_t>(count - 1), gap_to)) {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::RetransmitLimitViolation,
            .state = state_,
            .gap_from = from_seq_no,
            .summary = "RetransmitRequest range overflows uint64",
        });
        transition_to(TwimeSessionState::Faulted, "RetransmitRequest range overflows uint64");
        return;
    }

    if (count > max_retransmit_request_count()) {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::RetransmitLimitViolation,
            .state = state_,
            .gap_from = from_seq_no,
            .gap_to = gap_to,
            .summary = "RetransmitRequest exceeds fake-mode limit",
        });
        transition_to(TwimeSessionState::Faulted, "RetransmitRequest exceeds fake-mode limit");
        return;
    }

    if (send_message(build_request("RetransmitRequest", kTemplateRetransmitRequest,
                                   {
                                       {"Timestamp", TwimeFieldValue::timestamp(clock_.now_ns())},
                                       {"FromSeqNo", TwimeFieldValue::unsigned_integer(from_seq_no)},
                                       {"Count", TwimeFieldValue::unsigned_integer(count)},
                                   }),
                     TwimeSessionEventType::RetransmitRequested, "RetransmitRequest sent")) {
        start_pending_retransmission(from_seq_no, count);
    }
}

void TwimeSession::process_transport_event(const TwimeFakeTransportEvent& event) {
    if (event.kind == TwimeFakeTransportEventKind::PeerClosed) {
        handle_peer_close("fake peer closed transport");
        return;
    }

    process_inbound_frame(event.frame);
}

void TwimeSession::handle_peer_close(std::string peer_summary) {
    if (fake_transport_ != nullptr) {
        fake_transport_->disconnect();
    }

    if (state_ == TwimeSessionState::Terminated && terminate_response_received_) {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::PeerClosedCleanAfterTerminateResponse,
            .state = state_,
            .summary = peer_summary + " after inbound Terminate(Finished)",
        });
    } else if (state_ == TwimeSessionState::Terminating) {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::PeerClosedUnexpectedWhileTerminating,
            .state = state_,
            .summary = peer_summary + " before inbound Terminate(Finished)",
        });
        transition_to(TwimeSessionState::Faulted, "peer close while Terminating without Terminate(Finished)");
    } else {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::PeerClosed,
            .state = state_,
            .summary = std::move(peer_summary),
        });
        transition_to(TwimeSessionState::Faulted, "unexpected peer close");
    }
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

    const bool inbound_consumes_sequence = consumes_inbound_sequence(decoded);
    const auto expected_inbound_before = sequence_state_.next_expected_inbound_seq();
    std::uint64_t inbound_sequence_number = frame.sequence_number;
    if (inbound_consumes_sequence && inbound_sequence_number == 0 && byte_transport_ != nullptr) {
        inbound_sequence_number = expected_inbound_before;
    }
    if (inbound_consumes_sequence && inbound_sequence_number == 0) {
        transition_to(TwimeSessionState::Faulted, "fake application message missing sequence number");
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::Faulted,
            .state = state_,
            .template_id = decoded.header.template_id,
            .message_name = std::string(decoded.metadata->name),
            .summary = "fake application message missing sequence number",
        });
        return;
    }

    if (inbound_consumes_sequence) {
        const auto observation = sequence_state_.observe_inbound(inbound_sequence_number, true);
        recovery_state_.next_expected_inbound_seq = sequence_state_.next_expected_inbound_seq();
        persist_recovery_state();

        if (observation.gap.has_value()) {
            transition_to(TwimeSessionState::Recovering, "inbound sequence gap detected");
            append_event(TwimeSessionEvent{
                .type = TwimeSessionEventType::SequenceGapDetected,
                .state = state_,
                .template_id = decoded.header.template_id,
                .sequence_number = inbound_sequence_number,
                .gap_from = observation.gap->from_seq_no,
                .gap_to = observation.gap->to_seq_no,
                .message_name = std::string(decoded.metadata->name),
                .summary = "inbound sequence gap detected",
            });
            if (config_.auto_request_retransmit_on_gap) {
                request_missing_messages(observation.gap->from_seq_no, observation.gap->to_seq_no,
                                         "inbound sequence gap detected");
            }
        }

        if (pending_retransmission_.active && observation.accepted) {
            if (!pending_retransmission_.metadata_received) {
                append_event(TwimeSessionEvent{
                    .type = TwimeSessionEventType::RetransmissionProtocolViolation,
                    .state = state_,
                    .template_id = decoded.header.template_id,
                    .sequence_number = inbound_sequence_number,
                    .message_name = std::string(decoded.metadata->name),
                    .summary = "retransmitted application message arrived before Retransmission metadata",
                });
                transition_to(TwimeSessionState::Faulted,
                              "retransmitted application message arrived before Retransmission metadata");
                return;
            } else if (expected_inbound_before != pending_retransmission_.expected_next_seq) {
                append_event(TwimeSessionEvent{
                    .type = TwimeSessionEventType::RetransmissionProtocolViolation,
                    .state = state_,
                    .template_id = decoded.header.template_id,
                    .sequence_number = inbound_sequence_number,
                    .message_name = std::string(decoded.metadata->name),
                    .summary = "retransmitted application message sequence does not match pending window",
                });
                transition_to(TwimeSessionState::Faulted,
                              "retransmitted application message sequence does not match pending window");
                return;
            } else {
                ++pending_retransmission_.received_retransmit_count;
                ++pending_retransmission_.expected_next_seq;
                if (pending_retransmission_.metadata_received &&
                    pending_retransmission_.received_retransmit_count == pending_retransmission_.requested_count) {
                    clear_pending_retransmission();
                    if (state_ == TwimeSessionState::Recovering) {
                        transition_to(TwimeSessionState::Active,
                                      "expected retransmitted application messages processed");
                    }
                }
            }
        }
    }

    const auto formatted = formatter_.format(decoded);
    inbound_journal_.append(TwimeJournalEntry{
        .sequence_number = inbound_sequence_number,
        .consumes_sequence = inbound_consumes_sequence,
        .recoverable = is_recoverable_message(decoded),
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
        .sequence_number = inbound_sequence_number,
        .message_name = std::string(decoded.metadata->name),
        .summary = "inbound message received",
        .cert_log_line = formatted,
    });

    process_inbound_message(decoded, frame);
}

void TwimeSession::process_inbound_message(const DecodedTwimeMessage& message, const TwimeFakeTransportFrame& frame) {
    switch (message.header.template_id) {
    case kTemplateEstablishmentAck: {
        const auto keepalive = field_unsigned(message, "KeepaliveInterval").value_or(0);
        if (!validate_keepalive_interval(keepalive)) {
            append_event(TwimeSessionEvent{
                .type = TwimeSessionEventType::KeepaliveIntervalRejected,
                .state = state_,
                .template_id = message.header.template_id,
                .sequence_number = frame.sequence_number,
                .reason_code = static_cast<std::int64_t>(keepalive),
                .message_name = std::string(message.metadata->name),
                .summary = "EstablishmentAck KeepaliveInterval out of range",
                .cert_log_line = formatter_.format(message),
            });
            transition_to(TwimeSessionState::Faulted, "EstablishmentAck KeepaliveInterval out of range");
            return;
        }

        active_keepalive_interval_ms_ = static_cast<std::uint32_t>(keepalive);
        heartbeat_due_ms_ = clock_.now_ms() + active_keepalive_interval_ms_;

        const auto expected_before = sequence_state_.next_expected_inbound_seq();
        const auto ack_next_seq = field_unsigned(message, "NextSeqNo").value_or(expected_before);
        bool waiting_for_retransmission = false;

        if (ack_next_seq < expected_before) {
            sequence_state_.restore_expected_inbound_next(ack_next_seq);
            recovery_state_.next_expected_inbound_seq = ack_next_seq;
            persist_recovery_state();
            append_event(TwimeSessionEvent{
                .type = TwimeSessionEventType::MessageCounterResetDetected,
                .state = state_,
                .template_id = message.header.template_id,
                .sequence_number = frame.sequence_number,
                .gap_from = expected_before,
                .gap_to = ack_next_seq,
                .message_name = std::string(message.metadata->name),
                .summary = "EstablishmentAck NextSeqNo indicates message counter reset",
                .cert_log_line = formatter_.format(message),
            });
        } else if (recovering_from_dirty_snapshot_ && ack_next_seq > expected_before) {
            transition_to(TwimeSessionState::Recovering, "EstablishmentAck indicates missing inbound messages");
            append_event(TwimeSessionEvent{
                .type = TwimeSessionEventType::SequenceGapDetected,
                .state = state_,
                .template_id = message.header.template_id,
                .gap_from = expected_before,
                .gap_to = ack_next_seq - 1,
                .message_name = std::string(message.metadata->name),
                .summary = "EstablishmentAck NextSeqNo indicates missing inbound messages",
                .cert_log_line = formatter_.format(message),
            });
            waiting_for_retransmission =
                request_missing_messages(expected_before, ack_next_seq - 1, "EstablishmentAck gap reconciliation");
        } else {
            sequence_state_.restore_expected_inbound_next(ack_next_seq);
            recovery_state_.next_expected_inbound_seq = sequence_state_.next_expected_inbound_seq();
            persist_recovery_state();
        }

        recovering_from_dirty_snapshot_ = false;
        if (state_ != TwimeSessionState::Faulted && !waiting_for_retransmission) {
            transition_to(TwimeSessionState::Active, "EstablishmentAck received");
        }
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
    case kTemplateTerminate: {
        const auto termination_code = field_unsigned(message, "TerminationCode").value_or(255);
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::TerminateReceived,
            .state = state_,
            .template_id = message.header.template_id,
            .sequence_number = frame.sequence_number,
            .reason_code = static_cast<std::int64_t>(termination_code),
            .message_name = std::string(message.metadata->name),
            .summary = "Terminate received",
            .cert_log_line = formatter_.format(message),
        });
        if (termination_code == 0) {
            terminate_response_received_ = true;
            recovery_state_.last_clean_shutdown = true;
            persist_recovery_state();
            transition_to(TwimeSessionState::Terminated, "inbound Terminate(Finished) received");
        } else {
            transition_to(TwimeSessionState::Faulted, "inbound Terminate has non-Finished code");
        }
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
                    request_missing_messages(gap->from_seq_no, gap->to_seq_no, "peer NextSeqNo indicates a gap");
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
        const auto next_seq = field_unsigned(message, "NextSeqNo");
        const auto count = field_unsigned(message, "Count");
        if (!next_seq.has_value() || !count.has_value() ||
            *count > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) ||
            !validate_retransmission_window(*next_seq, static_cast<std::uint32_t>(*count), message, frame)) {
            return;
        }
        pending_retransmission_.metadata_received = true;
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

bool TwimeSession::request_missing_messages(std::uint64_t from_seq_no, std::uint64_t to_seq_no,
                                            std::string /*summary*/) {
    if (to_seq_no < from_seq_no) {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::RetransmitLimitViolation,
            .state = state_,
            .gap_from = from_seq_no,
            .gap_to = to_seq_no,
            .summary = "invalid retransmission range",
        });
        transition_to(TwimeSessionState::Faulted, "invalid retransmission range");
        return false;
    }

    const auto count64 = (to_seq_no - from_seq_no) + 1ULL;
    if (count64 > static_cast<std::uint64_t>(max_retransmit_request_count()) ||
        count64 > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::RetransmitLimitViolation,
            .state = state_,
            .gap_from = from_seq_no,
            .gap_to = to_seq_no,
            .summary = "retransmission range exceeds supported fake-session limits",
        });
        transition_to(TwimeSessionState::Faulted, "retransmission range exceeds supported fake-session limits");
        return false;
    }

    send_retransmit_request(from_seq_no, static_cast<std::uint32_t>(count64));
    return state_ != TwimeSessionState::Faulted;
}

bool TwimeSession::validate_keepalive_interval(std::uint64_t value) {
    return value >= 1000 && value <= 60000;
}

bool TwimeSession::is_recoverable_message(const DecodedTwimeMessage& message) const noexcept {
    return message.metadata != nullptr && message.metadata->layer == TwimeLayer::Application &&
           message.header.template_id != kTemplateBusinessReject;
}

bool TwimeSession::consumes_inbound_sequence(const DecodedTwimeMessage& message) const noexcept {
    return message.metadata != nullptr && message.metadata->layer == TwimeLayer::Application &&
           message.metadata->direction != TwimeDirection::ClientToServer;
}

std::uint32_t TwimeSession::max_retransmit_request_count() const noexcept {
    switch (config_.recovery_mode) {
    case TwimeRecoveryMode::NormalSessionRecovery:
        return 10;
    case TwimeRecoveryMode::FullRecoveryService:
        return 1000;
    }
    return 10;
}

bool TwimeSession::is_reconnect_too_fast() const noexcept {
    return last_connect_attempt_ms_ != 0 && clock_.now_ms() < last_connect_attempt_ms_ + 1000;
}

void TwimeSession::mark_connect_attempt() noexcept {
    last_connect_attempt_ms_ = clock_.now_ms();
}

void TwimeSession::start_pending_retransmission(std::uint64_t from_seq_no, std::uint32_t count) {
    std::uint64_t expected_final_next_seq = 0;
    if (!try_add_u64(from_seq_no, static_cast<std::uint64_t>(count), expected_final_next_seq)) {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::RetransmitLimitViolation,
            .state = state_,
            .gap_from = from_seq_no,
            .summary = "pending retransmission window overflowed uint64",
        });
        transition_to(TwimeSessionState::Faulted, "pending retransmission window overflowed uint64");
        return;
    }

    pending_retransmission_ = TwimePendingRetransmissionWindow{
        .active = true,
        .metadata_received = false,
        .requested_from_seq = from_seq_no,
        .requested_count = count,
        .received_retransmit_count = 0,
        .expected_next_seq = from_seq_no,
        .expected_final_next_seq = expected_final_next_seq,
    };
}

void TwimeSession::clear_pending_retransmission() noexcept {
    pending_retransmission_ = TwimePendingRetransmissionWindow{};
}

bool TwimeSession::validate_retransmission_window(std::uint64_t next_seq_no, std::uint32_t count,
                                                  const DecodedTwimeMessage& message,
                                                  const TwimeFakeTransportFrame& frame) {
    if (!pending_retransmission_.active) {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::RetransmissionProtocolViolation,
            .state = state_,
            .template_id = message.header.template_id,
            .sequence_number = frame.sequence_number,
            .message_name = std::string(message.metadata->name),
            .summary = "Retransmission metadata received without a pending request",
            .cert_log_line = formatter_.format(message),
        });
        transition_to(TwimeSessionState::Faulted, "Retransmission metadata received without a pending request");
        return false;
    }

    if (count != pending_retransmission_.requested_count ||
        next_seq_no != pending_retransmission_.expected_final_next_seq) {
        append_event(TwimeSessionEvent{
            .type = TwimeSessionEventType::RetransmissionProtocolViolation,
            .state = state_,
            .template_id = message.header.template_id,
            .sequence_number = frame.sequence_number,
            .gap_from = pending_retransmission_.requested_from_seq,
            .gap_to = pending_retransmission_.expected_final_next_seq,
            .message_name = std::string(message.metadata->name),
            .summary = "Retransmission metadata does not match pending request",
            .cert_log_line = formatter_.format(message),
        });
        transition_to(TwimeSessionState::Faulted, "Retransmission metadata does not match pending request");
        return false;
    }

    return true;
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

bool TwimeSession::write_outbound_bytes(std::span<const std::byte> bytes) {
    if (byte_transport_ == nullptr) {
        return true;
    }

    if (!bytes.empty()) {
        pending_outbound_bytes_.insert(pending_outbound_bytes_.end(), bytes.begin(), bytes.end());
    }
    return flush_pending_outbound_bytes();
}

bool TwimeSession::flush_pending_outbound_bytes() {
    if (byte_transport_ == nullptr) {
        pending_outbound_bytes_.clear();
        pending_outbound_offset_ = 0;
        return true;
    }

    while (pending_outbound_offset_ < pending_outbound_bytes_.size()) {
        const auto result = byte_transport_->write(std::span<const std::byte>(
            pending_outbound_bytes_.data() + static_cast<std::ptrdiff_t>(pending_outbound_offset_),
            pending_outbound_bytes_.size() - pending_outbound_offset_));
        switch (result.status) {
        case transport::TwimeTransportStatus::Ok:
            if (result.bytes_transferred == 0) {
                transition_to(TwimeSessionState::Faulted, "byte-stream transport returned zero-byte write");
                append_event(TwimeSessionEvent{
                    .type = TwimeSessionEventType::Faulted,
                    .state = state_,
                    .summary = "byte-stream transport returned zero-byte successful write",
                });
                return false;
            }
            pending_outbound_offset_ += result.bytes_transferred;
            break;
        case transport::TwimeTransportStatus::WouldBlock:
            return true;
        case transport::TwimeTransportStatus::RemoteClosed:
            handle_peer_close("byte-stream transport remote close during write");
            return false;
        case transport::TwimeTransportStatus::Closed:
        case transport::TwimeTransportStatus::Fault:
        case transport::TwimeTransportStatus::InvalidState:
        case transport::TwimeTransportStatus::BufferTooSmall:
            transition_to(TwimeSessionState::Faulted, "byte-stream transport write fault");
            append_event(TwimeSessionEvent{
                .type = TwimeSessionEventType::Faulted,
                .state = state_,
                .summary = "byte-stream transport write fault",
            });
            return false;
        }
    }

    pending_outbound_bytes_.clear();
    pending_outbound_offset_ = 0;
    return true;
}

bool TwimeSession::send_message(const TwimeEncodeRequest& request, TwimeSessionEventType event_type,
                                std::string summary) {
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

    const auto classification = classify_outbound_message(*decoded.metadata);
    const auto rate_limit_decision = rate_limit_model_.observe_send(clock_.now_ms(), classification);
    if (!rate_limit_decision.allowed) {
        if (rate_limit_decision.heartbeat_rate_violation) {
            append_event(TwimeSessionEvent{
                .type = TwimeSessionEventType::HeartbeatRateViolation,
                .state = state_,
                .template_id = decoded.header.template_id,
                .message_name = std::string(decoded.metadata->name),
                .summary = "more than three heartbeat messages per second in fake model",
                .cert_log_line = formatter_.format(decoded),
            });
            transition_to(TwimeSessionState::Faulted, "heartbeat rate violation");
        } else {
            append_event(TwimeSessionEvent{
                .type = TwimeSessionEventType::Faulted,
                .state = state_,
                .template_id = decoded.header.template_id,
                .message_name = std::string(decoded.metadata->name),
                .summary = "rate-limit model rejected outbound message",
                .cert_log_line = formatter_.format(decoded),
            });
            transition_to(TwimeSessionState::Faulted, "rate-limit model rejected outbound message");
        }
        return false;
    }

    const bool consumes_sequence = decoded.metadata->layer == TwimeLayer::Application &&
                                   decoded.metadata->direction == TwimeDirection::ClientToServer;
    const auto transport_sequence = consumes_sequence ? sequence_state_.reserve_outbound_sequence(true) : 0;
    persist_recovery_state();

    const auto formatted = formatter_.format(decoded);
    if (fake_transport_ != nullptr) {
        fake_transport_->send_outbound_frame(TwimeFakeTransportFrame{
            .bytes = bytes,
            .sequence_number = transport_sequence,
            .consumes_sequence = consumes_sequence,
        });
    } else if (!write_outbound_bytes(bytes)) {
        return false;
    }
    outbound_journal_.append(TwimeJournalEntry{
        .sequence_number = transport_sequence,
        .consumes_sequence = consumes_sequence,
        .recoverable = false,
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

} // namespace moex::twime_trade
