#include "moex/twime_trade/twime_live_session_runner.hpp"

#include "moex/twime_trade/transport/twime_credential_redaction.hpp"
#include "moex/twime_trade/transport/twime_endpoint_resolver.hpp"
#include "moex/twime_trade/transport/twime_test_network_gate.hpp"

#include <sstream>

namespace moex::twime_trade {

namespace {

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

bool is_external_endpoint(const transport::TwimeTcpConfig& config) {
    return !transport::twime_is_explicit_loopback_host(config.endpoint.host);
}

} // namespace

TwimeLiveSessionRunner::TwimeLiveSessionRunner(TwimeLiveSessionConfig config,
                                               TwimeSessionPersistenceStore& persistence_store,
                                               TwimeFakeClock& session_clock)
    : config_(std::move(config)), persistence_store_(persistence_store), recovery_store_(persistence_store_),
      session_clock_(session_clock), transport_(config_.tcp),
      session_(config_.session, transport_, recovery_store_, session_clock_) {
    health_ = make_twime_session_health_snapshot(session_, false, transport_.metrics(), metrics_);
}

void TwimeLiveSessionRunner::set_time_source(TimeSource time_source) {
    time_source_ = std::move(time_source);
    transport_.set_time_source(time_source_);
}

TwimeLiveSessionRunResult TwimeLiveSessionRunner::start() {
    if (started_) {
        return fail("live session runner already started");
    }

    if (const auto gate = validate_operator_arms(); !gate.ok) {
        return gate;
    }
    if (const auto creds = load_credentials(); !creds.ok) {
        return creds;
    }

    const auto snapshot = persistence_store_.load(config_.session.session_id);
    if (snapshot.has_value()) {
        append_operator_log("loaded persisted session snapshot");
    }

    external_session_path_ = is_external_endpoint(config_.tcp);
    append_operator_log(transport::format_twime_test_network_banner(config_.tcp.runtime_arm_state.test_network_armed));
    if (config_.tcp.runtime_arm_state.test_session_armed) {
        append_operator_log("[TEST-SESSION-ARMED]");
    }
    append_operator_log("endpoint=" + config_.tcp.endpoint.host + ":" + std::to_string(config_.tcp.endpoint.port));
    append_operator_log("credentials_source=" +
                        std::string(config_.credentials.source == transport::TwimeCredentialSource::Env    ? "env"
                                    : config_.credentials.source == transport::TwimeCredentialSource::File ? "file"
                                                                                                           : "none"));

    ++metrics_.reconnect_attempts;
    started_ = true;
    session_.apply_command({.type = TwimeSessionCommandType::ConnectFake});
    refresh_health();
    return {.ok = true, .message = "live session start requested"};
}

TwimeLiveSessionRunResult TwimeLiveSessionRunner::poll_once() {
    if (!started_) {
        return fail("live session runner not started");
    }

    sync_fake_clock();
    maybe_attempt_reconnect();
    session_.poll_transport();
    session_.on_timer_tick();

    auto events = session_.drain_events();
    update_twime_session_metrics(metrics_, events, session_clock_.now_ms());
    handle_session_events(events);
    refresh_health();
    persist_snapshot();

    if (session_.state() == TwimeSessionState::Establishing && establish_sent_seen_ &&
        session_clock_.now_ms() > establish_started_ms_ + config_.policy.establish_deadline_ms) {
        append_operator_log("establish deadline exceeded");
        transport_.close();
        session_.force_fault("Establish deadline exceeded");
        refresh_health();
        persist_snapshot();
        maybe_schedule_reconnect();
        return {.ok = false, .message = "Establish deadline exceeded"};
    }

    if (session_.state() == TwimeSessionState::Faulted) {
        maybe_schedule_reconnect();
    }

    return {.ok = true, .message = "poll completed"};
}

TwimeLiveSessionRunResult TwimeLiveSessionRunner::request_stop() {
    stop_requested_ = true;
    if (session_.state() == TwimeSessionState::Active || session_.state() == TwimeSessionState::Recovering ||
        session_.state() == TwimeSessionState::Establishing) {
        session_.apply_command({.type = TwimeSessionCommandType::SendTerminate});
        terminate_started_ms_ = session_clock_.now_ms();
        append_operator_log("operator requested graceful terminate");
    } else {
        transport_.close();
    }
    refresh_health();
    return {.ok = true, .message = "stop requested"};
}

TwimeLiveSessionRunResult TwimeLiveSessionRunner::stop_if_needed() {
    if (!stop_requested_) {
        return {.ok = true, .message = "no stop requested"};
    }
    if (session_.state() == TwimeSessionState::Terminated || session_.state() == TwimeSessionState::Rejected ||
        session_.state() == TwimeSessionState::Faulted) {
        transport_.close();
        refresh_health();
        persist_snapshot();
        return {.ok = true, .message = "runner stopped"};
    }
    if (session_.state() == TwimeSessionState::Terminating &&
        session_clock_.now_ms() > terminate_started_ms_ + config_.policy.graceful_terminate_timeout_ms) {
        transport_.close();
        append_operator_log("graceful terminate timeout elapsed");
        refresh_health();
        persist_snapshot();
        return {.ok = true, .message = "terminate timeout reached"};
    }
    return {.ok = true, .message = "stop still in progress"};
}

bool TwimeLiveSessionRunner::reconnect_due() const noexcept {
    return reconnect_scheduled_ && session_clock_.now_ms() >= reconnect_due_ms_;
}

const TwimeSessionHealthSnapshot& TwimeLiveSessionRunner::health_snapshot() const noexcept {
    return health_;
}

const TwimeSessionMetrics& TwimeLiveSessionRunner::session_metrics() const noexcept {
    return metrics_;
}

const transport::TwimeTcpTransport& TwimeLiveSessionRunner::transport() const noexcept {
    return transport_;
}

const std::vector<std::string>& TwimeLiveSessionRunner::operator_log_lines() const noexcept {
    return operator_log_lines_;
}

const std::vector<std::string>& TwimeLiveSessionRunner::cert_log_lines() const noexcept {
    return session_.cert_log_lines();
}

const TwimeSession& TwimeLiveSessionRunner::session() const noexcept {
    return session_;
}

bool TwimeLiveSessionRunner::ready_for_application_order_flow() const noexcept {
    return false;
}

bool TwimeLiveSessionRunner::establish_deadline_expired() const noexcept {
    return session_.state() == TwimeSessionState::Establishing && establish_sent_seen_ &&
           session_clock_.now_ms() > establish_started_ms_ + config_.policy.establish_deadline_ms;
}

TwimeLiveSessionRunner::TimePoint TwimeLiveSessionRunner::now() const {
    return time_source_ ? time_source_() : std::chrono::steady_clock::now();
}

void TwimeLiveSessionRunner::sync_fake_clock() {
    const auto current = now();
    if (!has_last_time_point_) {
        last_time_point_ = current;
        has_last_time_point_ = true;
        return;
    }
    const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(current - last_time_point_);
    if (delta.count() > 0) {
        session_clock_.advance(static_cast<std::uint64_t>(delta.count()));
        last_time_point_ = current;
    }
}

void TwimeLiveSessionRunner::refresh_health() {
    const bool transport_open = transport_.state() == transport::TwimeTransportState::Open ||
                                transport_.state() == transport::TwimeTransportState::Opening;
    health_ = make_twime_session_health_snapshot(session_, transport_open, transport_.metrics(), metrics_);
}

void TwimeLiveSessionRunner::append_operator_log(std::string line) {
    operator_log_lines_.push_back(std::move(line));
}

void TwimeLiveSessionRunner::persist_snapshot() {
    persistence_store_.save(config_.session.session_id,
                            make_twime_session_persistence_snapshot(health_, session_.recovery_state()));
}

TwimeLiveSessionRunResult TwimeLiveSessionRunner::fail(std::string message) {
    append_operator_log("FAULT " + message);
    refresh_health();
    persist_snapshot();
    return {.ok = false, .message = std::move(message)};
}

void TwimeLiveSessionRunner::handle_session_events(std::span<const TwimeSessionEvent> events) {
    for (const auto& event : events) {
        switch (event.type) {
        case TwimeSessionEventType::OutboundMessage:
            append_operator_log("outbound " + event.message_name);
            if (event.message_name == "Establish") {
                establish_sent_seen_ = true;
                establish_started_ms_ = session_clock_.now_ms();
            }
            break;
        case TwimeSessionEventType::InboundMessage:
            if (event.message_name == "EstablishmentAck") {
                transport_open_seen_ = true;
                append_operator_log("EstablishmentAck received");
            } else if (event.message_name == "EstablishmentReject") {
                append_operator_log("EstablishmentReject received");
            }
            break;
        case TwimeSessionEventType::HeartbeatSent:
            append_operator_log("Sequence sent");
            break;
        case TwimeSessionEventType::HeartbeatReceived:
            append_operator_log("Sequence received");
            break;
        case TwimeSessionEventType::TerminateReceived:
            append_operator_log("Terminate received");
            break;
        case TwimeSessionEventType::EstablishmentRejected:
            append_operator_log("session rejected code=" + std::to_string(event.reason_code));
            break;
        case TwimeSessionEventType::Faulted:
            append_operator_log("session faulted: " + event.summary);
            break;
        case TwimeSessionEventType::PeerClosed:
        case TwimeSessionEventType::PeerClosedCleanAfterTerminateResponse:
        case TwimeSessionEventType::PeerClosedUnexpectedWhileTerminating:
            append_operator_log("peer close: " + event.summary);
            break;
        case TwimeSessionEventType::StateChanged:
            append_operator_log("state=" + state_name(event.state) + " " + event.summary);
            break;
        default:
            break;
        }
    }
}

bool TwimeLiveSessionRunner::should_schedule_reconnect() const noexcept {
    return config_.policy.reconnect_enabled && !stop_requested_ &&
           metrics_.reconnect_attempts < config_.policy.max_reconnect_attempts &&
           session_.state() == TwimeSessionState::Faulted;
}

void TwimeLiveSessionRunner::maybe_schedule_reconnect() {
    if (!should_schedule_reconnect() || reconnect_scheduled_) {
        return;
    }
    reconnect_scheduled_ = true;
    reconnect_due_ms_ = session_clock_.now_ms() + 1000;
    append_operator_log("Reconnect scheduled");
}

void TwimeLiveSessionRunner::maybe_attempt_reconnect() {
    if (!reconnect_due()) {
        return;
    }
    reconnect_scheduled_ = false;
    append_operator_log("Attempting reconnect");
    ++metrics_.reconnect_attempts;
    session_.apply_command({.type = TwimeSessionCommandType::ConnectFake});
}

TwimeLiveSessionRunResult TwimeLiveSessionRunner::validate_operator_arms() const {
    if (const auto connect_gate =
            TwimeManualOperatorGate::validate_transport_connect(config_.tcp, config_.tcp.runtime_arm_state);
        !connect_gate.allowed) {
        return {.ok = false, .message = connect_gate.reason};
    }
    if (const auto session_gate =
            TwimeManualOperatorGate::validate_session_start(config_.tcp, config_.tcp.runtime_arm_state);
        !session_gate.allowed) {
        return {.ok = false, .message = session_gate.reason};
    }
    return {.ok = true, .message = "operator gate passed"};
}

TwimeLiveSessionRunResult TwimeLiveSessionRunner::load_credentials() {
    const auto maybe_credentials = transport::load_twime_credentials(config_.credentials);
    if (!maybe_credentials.has_value()) {
        return {.ok = false, .message = "missing TWIME credentials from configured local source"};
    }
    config_.session.credentials = maybe_credentials->credentials;
    append_operator_log("credentials=" + transport::redact_twime_credentials(config_.session.credentials));
    return {.ok = true, .message = "credentials loaded"};
}

} // namespace moex::twime_trade
