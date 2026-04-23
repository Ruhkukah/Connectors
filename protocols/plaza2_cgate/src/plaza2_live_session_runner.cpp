#include "moex/plaza2/cgate/plaza2_live_session_runner.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moex::plaza2::cgate {

namespace {

using fake::EngineState;
using fake::EventKind;
using fake::EventSpec;
using fake::FieldValueSpec;
using fake::RowSpec;
using generated::FieldCode;
using generated::StreamCode;
using generated::TableCode;

constexpr std::string_view kCredentialToken = "${PLAZA2_TEST_CREDENTIALS}";
constexpr std::array<StreamCode, 5> kRequiredPrivateStreams = {
    StreamCode::kFortsTradeRepl,
    StreamCode::kFortsUserorderbookRepl,
    StreamCode::kFortsPosRepl,
    StreamCode::kFortsPartRepl,
    StreamCode::kFortsRefdataRepl,
};

bool is_loopback_host(std::string_view host) {
    std::string normalized(host);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized == "127.0.0.1" || normalized == "::1" || normalized == "localhost";
}

std::string stream_name(StreamCode stream_code) {
    const auto* descriptor = generated::FindStreamByCode(stream_code);
    return descriptor == nullptr ? std::string{} : std::string(descriptor->stream_name);
}

std::size_t stream_index(const EngineState& state, StreamCode stream_code) {
    for (std::size_t index = 0; index < state.streams.size(); ++index) {
        if (state.streams[index].stream_code == stream_code) {
            return index;
        }
    }
    return state.streams.size();
}

bool settings_need_credentials(std::string_view value) {
    return value.find(kCredentialToken) != std::string_view::npos;
}

std::string substitute_credentials(std::string_view value, std::string_view credential_value) {
    std::string rendered(value);
    std::size_t position = 0;
    while ((position = rendered.find(kCredentialToken, position)) != std::string::npos) {
        rendered.replace(position, kCredentialToken.size(), credential_value);
        position += credential_value.size();
    }
    return rendered;
}

std::string first_fatal_issue_message(const Plaza2RuntimeProbeReport& report) {
    for (const auto& issue : report.issues) {
        if (issue.fatal) {
            return issue.message;
        }
    }
    return "PLAZA II runtime probe reported an incompatible TEST runtime layout";
}

class LiveProjectorBridge final : public Plaza2ListenerEventHandler {
  public:
    explicit LiveProjectorBridge(private_state::Plaza2PrivateStateProjector& projector) : projector_(projector) {
        scenario_.scenario_id = "phase3f_live_private_state";
        scenario_.description = "Phase 3F live TEST bring-up bridge";
        scenario_.metadata_version = 1;
    }

    Plaza2Error reset(std::span<const Plaza2LiveStreamConfig> streams) {
        projector_.reset();
        state_ = {};
        state_.streams.clear();
        pending_row_deltas_.assign(streams.size(), 0);
        for (const auto& stream : streams) {
            state_.streams.push_back({
                .stream_code = stream.stream_code,
                .stream_name = generated::FindStreamByCode(stream.stream_code) == nullptr
                                   ? std::string_view{}
                                   : generated::FindStreamByCode(stream.stream_code)->stream_name,
            });
        }
        last_resync_reason_.clear();
        return {};
    }

    Plaza2Error begin_run() {
        state_.open = true;
        state_.closed = false;
        const EventSpec event{.kind = EventKind::kOpen};
        projector_.on_event(scenario_, event, state_);
        return {};
    }

    Plaza2Error end_run() {
        state_.closed = true;
        state_.online = false;
        state_.transaction_open = false;
        for (auto& stream : state_.streams) {
            stream.online = false;
        }
        const EventSpec event{.kind = EventKind::kClose};
        projector_.on_event(scenario_, event, state_);
        return {};
    }

    const EngineState& state() const noexcept {
        return state_;
    }

    const std::string& last_resync_reason() const noexcept {
        return last_resync_reason_;
    }

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        try {
            return handle_event(event);
        } catch (const std::exception& error) {
            state_.callback_error_count += 1;
            return {
                .code = Plaza2ErrorCode::CallbackFailed,
                .runtime_code = 0,
                .message = std::string("Phase 3F projector bridge failed: ") + error.what(),
            };
        } catch (...) {
            state_.callback_error_count += 1;
            return {
                .code = Plaza2ErrorCode::CallbackFailed,
                .runtime_code = 0,
                .message = "Phase 3F projector bridge failed with an unknown exception",
            };
        }
    }

  private:
    Plaza2Error handle_event(const Plaza2ListenerEvent& event) {
        switch (event.kind) {
        case Plaza2ListenerEventKind::Open:
        case Plaza2ListenerEventKind::Timeout:
            return {};
        case Plaza2ListenerEventKind::Close:
            return handle_close(event.stream_code);
        case Plaza2ListenerEventKind::TransactionBegin:
            return handle_transaction_begin(event.stream_code);
        case Plaza2ListenerEventKind::TransactionCommit:
            return handle_transaction_commit(event.stream_code);
        case Plaza2ListenerEventKind::StreamData:
            return handle_stream_data(event);
        case Plaza2ListenerEventKind::Online:
            return handle_online(event.stream_code);
        case Plaza2ListenerEventKind::LifeNum:
            return handle_lifenum(event.unsigned_value);
        case Plaza2ListenerEventKind::ClearDeleted:
            return handle_clear_deleted(event.stream_code);
        case Plaza2ListenerEventKind::ReplState:
            return handle_replstate(event.text_value);
        }
        return {};
    }

    Plaza2Error handle_close(StreamCode stream_code) {
        const auto index = stream_index(state_, stream_code);
        if (index < state_.streams.size()) {
            state_.streams[index].online = false;
        }
        recompute_online();
        return {};
    }

    Plaza2Error handle_transaction_begin(StreamCode stream_code) {
        if (state_.transaction_open) {
            return ordering_error("PLAZA II live bridge received nested TN_BEGIN");
        }
        const auto index = stream_index(state_, stream_code);
        if (index == state_.streams.size()) {
            return ordering_error("PLAZA II live bridge received TN_BEGIN for an undeclared stream");
        }
        std::fill(pending_row_deltas_.begin(), pending_row_deltas_.end(), 0);
        state_.transaction_open = true;
        const EventSpec fake_event{
            .kind = EventKind::kTransactionBegin,
            .stream_code = stream_code,
        };
        projector_.on_event(scenario_, fake_event, state_);
        return {};
    }

    Plaza2Error handle_transaction_commit(StreamCode stream_code) {
        if (!state_.transaction_open) {
            return ordering_error("PLAZA II live bridge received TN_COMMIT without TN_BEGIN");
        }
        const auto index = stream_index(state_, stream_code);
        if (index == state_.streams.size()) {
            return ordering_error("PLAZA II live bridge received TN_COMMIT for an undeclared stream");
        }
        for (std::size_t delta_index = 0; delta_index < state_.streams.size(); ++delta_index) {
            state_.streams[delta_index].committed_row_count += pending_row_deltas_[delta_index];
        }
        std::fill(pending_row_deltas_.begin(), pending_row_deltas_.end(), 0);
        state_.transaction_open = false;
        state_.commit_count += 1;

        const EventSpec fake_event{
            .kind = EventKind::kTransactionCommit,
            .stream_code = stream_code,
        };
        projector_.on_event(scenario_, fake_event, state_);
        projector_.on_transaction_commit(scenario_, fake_event, state_);
        return {};
    }

    Plaza2Error handle_stream_data(const Plaza2ListenerEvent& event) {
        if (!state_.transaction_open) {
            return ordering_error("PLAZA II live bridge received STREAM_DATA outside TN_BEGIN/TN_COMMIT");
        }
        const auto index = stream_index(state_, event.stream_code);
        if (index == state_.streams.size()) {
            return ordering_error("PLAZA II live bridge received STREAM_DATA for an undeclared stream");
        }

        text_storage_.clear();
        field_storage_.clear();
        text_storage_.reserve(event.fields.size());
        field_storage_.reserve(event.fields.size());
        for (const auto& field : event.fields) {
            FieldValueSpec decoded{
                .field_code = field.field_code,
            };
            switch (field.kind) {
            case Plaza2DecodedValueKind::None:
                continue;
            case Plaza2DecodedValueKind::SignedInteger:
                decoded.kind = fake::ValueKind::kSignedInteger;
                decoded.signed_value = field.signed_value;
                break;
            case Plaza2DecodedValueKind::UnsignedInteger:
                decoded.kind = fake::ValueKind::kUnsignedInteger;
                decoded.unsigned_value = field.unsigned_value;
                break;
            case Plaza2DecodedValueKind::Decimal:
                decoded.kind = fake::ValueKind::kDecimal;
                text_storage_.emplace_back(field.text_value);
                decoded.text_value = text_storage_.back();
                break;
            case Plaza2DecodedValueKind::FloatingPoint:
                decoded.kind = fake::ValueKind::kFloatingPoint;
                text_storage_.emplace_back(field.text_value);
                decoded.text_value = text_storage_.back();
                break;
            case Plaza2DecodedValueKind::String:
                decoded.kind = fake::ValueKind::kString;
                text_storage_.emplace_back(field.text_value);
                decoded.text_value = text_storage_.back();
                break;
            case Plaza2DecodedValueKind::Timestamp:
                decoded.kind = fake::ValueKind::kTimestamp;
                decoded.unsigned_value = field.unsigned_value;
                break;
            }
            field_storage_.push_back(std::move(decoded));
        }

        const EventSpec fake_event{
            .kind = EventKind::kStreamData,
            .stream_code = event.stream_code,
            .table_code = event.table_code,
        };
        const RowSpec row{
            .stream_code = event.stream_code,
            .table_code = event.table_code,
            .field_count = static_cast<std::uint32_t>(field_storage_.size()),
        };
        projector_.on_stream_row(scenario_, fake_event, row, field_storage_, state_);
        pending_row_deltas_[index] += 1;
        return {};
    }

    Plaza2Error handle_online(StreamCode stream_code) {
        const auto index = stream_index(state_, stream_code);
        if (index == state_.streams.size()) {
            return ordering_error("PLAZA II live bridge received ONLINE for an undeclared stream");
        }
        state_.streams[index].online = true;
        state_.streams[index].snapshot_complete = true;
        recompute_online();
        const EventSpec fake_event{
            .kind = EventKind::kOnline,
            .stream_code = stream_code,
        };
        projector_.on_event(scenario_, fake_event, state_);
        return {};
    }

    Plaza2Error handle_lifenum(std::uint64_t life_number) {
        if (state_.transaction_open) {
            return ordering_error("PLAZA II live bridge received P2REPL_LIFENUM inside an open transaction");
        }
        if (state_.has_lifenum && state_.last_lifenum == life_number) {
            return {};
        }
        if (state_.has_lifenum && state_.last_lifenum != life_number) {
            for (auto& stream : state_.streams) {
                stream.online = false;
                stream.snapshot_complete = false;
                stream.committed_row_count = 0;
            }
            state_.online = false;
            last_resync_reason_ = "lifenum_change";
        }
        state_.has_lifenum = true;
        state_.last_lifenum = life_number;
        const EventSpec fake_event{
            .kind = EventKind::kLifeNum,
            .numeric_value = life_number,
        };
        projector_.on_event(scenario_, fake_event, state_);
        return {};
    }

    Plaza2Error handle_clear_deleted(StreamCode stream_code) {
        if (state_.transaction_open) {
            return ordering_error("PLAZA II live bridge received P2REPL_CLEARDELETED inside an open transaction");
        }
        const auto index = stream_index(state_, stream_code);
        if (index == state_.streams.size()) {
            return ordering_error("PLAZA II live bridge received P2REPL_CLEARDELETED for an undeclared stream");
        }
        state_.streams[index].clear_deleted_count += 1;
        last_resync_reason_ = "clear_deleted:" + stream_name(stream_code);
        const EventSpec fake_event{
            .kind = EventKind::kClearDeleted,
            .stream_code = stream_code,
        };
        projector_.on_event(scenario_, fake_event, state_);
        return {};
    }

    Plaza2Error handle_replstate(std::string_view replstate) {
        if (state_.transaction_open) {
            return ordering_error("PLAZA II live bridge received P2REPL_REPLSTATE inside an open transaction");
        }
        state_.last_replstate.assign(replstate);
        const EventSpec fake_event{
            .kind = EventKind::kReplState,
            .text_value = state_.last_replstate,
        };
        projector_.on_event(scenario_, fake_event, state_);
        return {};
    }

    void recompute_online() {
        state_.online = !state_.streams.empty() &&
                        std::all_of(state_.streams.begin(), state_.streams.end(),
                                    [](const fake::StreamState& stream) { return stream.online; });
    }

    Plaza2Error ordering_error(std::string message) const {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = 0,
            .message = std::move(message),
        };
    }

    private_state::Plaza2PrivateStateProjector& projector_;
    fake::ScenarioSpec scenario_{};
    EngineState state_{};
    std::vector<std::uint64_t> pending_row_deltas_;
    std::vector<std::string> text_storage_;
    std::vector<FieldValueSpec> field_storage_;
    std::string last_resync_reason_;
};

struct LiveListenerHandle {
    Plaza2LiveStreamConfig config;
    Plaza2Listener listener;
};

} // namespace

struct Plaza2LiveSessionRunner::Impl {
    explicit Impl(Plaza2LiveSessionConfig initial_config)
        : config(std::move(initial_config)), projector_bridge(projector) {
        health.streams.reserve(config.streams.size());
        for (const auto& stream : config.streams) {
            health.streams.push_back({
                .stream_code = stream.stream_code,
                .stream_name = stream_name(stream.stream_code),
            });
        }
    }

    Plaza2LiveRunResult start() {
        if (started) {
            return fail("PLAZA II live TEST runner already started");
        }

        append_operator_log("profile=" + config.profile_id);
        append_operator_log("endpoint=" + config.endpoint_host + ":" + std::to_string(config.endpoint_port));

        if (const auto validation = validate_config(); !validation.ok) {
            return validation;
        }
        if (const auto transport_gate =
                Plaza2ManualOperatorGate::validate_transport_connect(config.endpoint_host, config.arm_state);
            !transport_gate.allowed) {
            return fail(transport_gate.reason);
        }
        if (const auto session_gate =
                Plaza2ManualOperatorGate::validate_session_start(config.endpoint_host, config.arm_state);
            !session_gate.allowed) {
            return fail(session_gate.reason);
        }

        if (const auto credentials = load_credentials_if_needed(); !credentials.ok) {
            return credentials;
        }

        effective_runtime = config.runtime;
        effective_runtime.env_open_settings = render_setting(config.runtime.env_open_settings);
        effective_connection_settings = render_setting(config.connection_settings);
        effective_connection_open_settings = render_setting(config.connection_open_settings);

        effective_streams.clear();
        effective_streams.reserve(config.streams.size());
        for (const auto& stream : config.streams) {
            effective_streams.push_back({
                .stream_code = stream.stream_code,
                .settings = render_setting(stream.settings),
                .open_settings = render_setting(stream.open_settings),
            });
        }

        health.state = Plaza2LiveRunnerState::Validated;
        probe_report = Plaza2RuntimeProbe::probe(effective_runtime);
        health.compatibility = probe_report.compatibility;
        health.runtime_probe_ok = probe_report.runtime_library_loadable;
        health.scheme_drift_ok = probe_report.scheme_drift.compatibility == Plaza2Compatibility::Compatible;
        if (probe_report.compatibility != Plaza2Compatibility::Compatible) {
            return fail(first_fatal_issue_message(probe_report));
        }
        append_operator_log("runtime_probe=compatible");
        append_operator_log("scheme_drift=compatible");

        if (const auto bridge_reset = projector_bridge.reset(effective_streams); bridge_reset) {
            return fail(bridge_reset.message);
        }
        if (const auto bridge_open = projector_bridge.begin_run(); bridge_open) {
            return fail(bridge_open.message);
        }

        if (const auto env_error = env.open(effective_runtime); env_error) {
            return fail(env_error.message);
        }
        append_operator_log("env=open");

        if (const auto connection_error = connection.create(env, effective_connection_settings); connection_error) {
            return fail(connection_error.message);
        }
        append_operator_log("connection=create");

        if (const auto open_error = connection.open(effective_connection_open_settings); open_error) {
            return fail(open_error.message);
        }
        append_operator_log("connection=open");

        listeners.clear();
        listeners.reserve(effective_streams.size());
        for (const auto& stream : effective_streams) {
            LiveListenerHandle handle{
                .config = stream,
            };
            if (const auto create_error =
                    handle.listener.create(connection, stream.stream_code, stream.settings, &projector_bridge);
                create_error) {
                return fail(create_error.message);
            }
            mark_stream_created(stream.stream_code);
            append_operator_log("listener=create stream=" + stream_name(stream.stream_code));

            if (const auto open_error = handle.listener.open(stream.open_settings); open_error) {
                return fail(open_error.message);
            }
            mark_stream_opened(stream.stream_code);
            append_operator_log("listener=open stream=" + stream_name(stream.stream_code));
            listeners.push_back(std::move(handle));
        }

        started = true;
        health.state = Plaza2LiveRunnerState::Started;
        refresh_health();
        return {
            .ok = true,
            .message = "PLAZA II TEST runner started",
        };
    }

    Plaza2LiveRunResult poll_once() {
        if (!started) {
            return fail("PLAZA II live TEST runner is not started");
        }

        std::uint32_t runtime_code = 0;
        const auto process_error = connection.process(config.process_timeout_ms, &runtime_code);
        health.last_process_runtime_code = runtime_code;
        if (process_error) {
            return fail(process_error.message);
        }

        refresh_health();
        if (health.ready) {
            append_operator_log("state=ready");
            health.state = Plaza2LiveRunnerState::Ready;
        }
        return {
            .ok = true,
            .message = "PLAZA II TEST runner poll completed",
        };
    }

    Plaza2LiveRunResult stop() {
        if (!started && health.state != Plaza2LiveRunnerState::Failed) {
            health.state = Plaza2LiveRunnerState::Stopped;
            return {
                .ok = true,
                .message = "PLAZA II TEST runner already stopped",
            };
        }

        for (auto it = listeners.rbegin(); it != listeners.rend(); ++it) {
            static_cast<void>(it->listener.close());
            static_cast<void>(it->listener.destroy());
        }
        listeners.clear();
        static_cast<void>(connection.close());
        static_cast<void>(connection.destroy());
        static_cast<void>(env.close());
        static_cast<void>(projector_bridge.end_run());
        started = false;
        refresh_health();
        health.state = Plaza2LiveRunnerState::Stopped;
        append_operator_log("runner=stopped");
        return {
            .ok = true,
            .message = "PLAZA II TEST runner stopped",
        };
    }

    Plaza2LiveRunResult validate_config() {
        if (config.profile_id.empty()) {
            return fail("profile_id must be set for PLAZA II TEST bring-up");
        }
        if (config.endpoint_host.empty()) {
            return fail("endpoint_host must be set for PLAZA II TEST bring-up");
        }
        if (config.runtime.environment != Plaza2Environment::Test) {
            return fail("Phase 3F PLAZA II bring-up is TEST-only");
        }
        if (const auto runtime_error = validate_plaza2_settings(config.runtime); runtime_error) {
            return fail(runtime_error.message);
        }
        if (config.runtime.env_open_settings.empty()) {
            return fail("runtime.env_open_settings must be provided explicitly");
        }
        if (config.connection_settings.empty()) {
            return fail("connection_settings must be provided explicitly");
        }
        if (config.streams.size() != kRequiredPrivateStreams.size()) {
            return fail("Phase 3F requires exactly the five private replication streams");
        }

        std::vector<StreamCode> present_streams;
        present_streams.reserve(config.streams.size());
        for (const auto& stream : config.streams) {
            if (stream.settings.empty()) {
                return fail("every configured PLAZA II listener must provide settings");
            }
            present_streams.push_back(stream.stream_code);
        }
        std::sort(present_streams.begin(), present_streams.end());

        auto expected_streams = std::vector<StreamCode>(kRequiredPrivateStreams.begin(), kRequiredPrivateStreams.end());
        std::sort(expected_streams.begin(), expected_streams.end());
        if (present_streams != expected_streams) {
            return fail("Phase 3F stream set must be exactly FORTS_TRADE_REPL, FORTS_USERORDERBOOK_REPL, "
                        "FORTS_POS_REPL, FORTS_PART_REPL, and FORTS_REFDATA_REPL");
        }
        return {
            .ok = true,
            .message = "PLAZA II TEST config validated",
        };
    }

    Plaza2LiveRunResult load_credentials_if_needed() {
        const bool needs_credentials =
            settings_need_credentials(config.runtime.env_open_settings) || settings_need_credentials(config.connection_settings) ||
            settings_need_credentials(config.connection_open_settings) ||
            std::any_of(config.streams.begin(), config.streams.end(), [](const Plaza2LiveStreamConfig& stream) {
                return settings_need_credentials(stream.settings) || settings_need_credentials(stream.open_settings);
            });

        if (config.credentials.source == Plaza2CredentialSource::None) {
            if (needs_credentials) {
                return fail("PLAZA II TEST settings require ${PLAZA2_TEST_CREDENTIALS}, but no credential source was configured");
            }
            return {
                .ok = true,
                .message = "PLAZA II TEST credentials not required",
            };
        }

        loaded_credentials = load_plaza2_credentials(config.credentials);
        if (!loaded_credentials.has_value()) {
            return fail(config.credentials.source == Plaza2CredentialSource::Env
                            ? "PLAZA II TEST credential env var is missing or empty"
                            : "PLAZA II TEST credential file is missing or empty");
        }

        append_operator_log("credentials_source=" +
                            std::string(config.credentials.source == Plaza2CredentialSource::Env ? "env" : "file"));
        append_operator_log("credentials=" + redact_plaza2_credentials(loaded_credentials->value));
        return {
            .ok = true,
            .message = "PLAZA II TEST credentials loaded",
        };
    }

    std::string render_setting(std::string_view value) const {
        if (!loaded_credentials.has_value()) {
            return std::string(value);
        }
        return substitute_credentials(value, loaded_credentials->value);
    }

    void mark_stream_created(StreamCode stream_code) {
        for (auto& status : health.streams) {
            if (status.stream_code == stream_code) {
                status.created = true;
            }
        }
    }

    void mark_stream_opened(StreamCode stream_code) {
        for (auto& status : health.streams) {
            if (status.stream_code == stream_code) {
                status.opened = true;
            }
        }
    }

    void refresh_health() {
        health.connector_health = projector.connector_health();
        health.resume_markers = projector.resume_markers();
        health.last_resync_reason = projector_bridge.last_resync_reason();

        for (auto& status : health.streams) {
            status.online = false;
            status.snapshot_complete = false;
        }
        for (const auto& stream_health : projector.stream_health()) {
            for (auto& status : health.streams) {
                if (status.stream_code == stream_health.stream_code) {
                    status.online = stream_health.online;
                    status.snapshot_complete = stream_health.snapshot_complete;
                }
            }
        }

        health.counts.session_count = projector.sessions().size();
        health.counts.instrument_count = projector.instruments().size();
        health.counts.matching_map_count = projector.matching_map().size();
        health.counts.limit_count = projector.limits().size();
        health.counts.position_count = projector.positions().size();
        health.counts.own_order_count = projector.own_orders().size();
        health.counts.own_trade_count = projector.own_trades().size();
        health.ready = health.runtime_probe_ok && health.scheme_drift_ok &&
                       std::all_of(health.streams.begin(), health.streams.end(), [](const Plaza2LiveStreamStatus& stream) {
                           return stream.created && stream.opened && stream.online && stream.snapshot_complete;
                       });
    }

    Plaza2LiveRunResult fail(std::string message) {
        health.state = Plaza2LiveRunnerState::Failed;
        health.last_error = message;
        append_operator_log("error=" + message);
        return {
            .ok = false,
            .message = std::move(message),
        };
    }

    void append_operator_log(std::string line) {
        if (operator_log_lines.empty() || operator_log_lines.back() != line) {
            operator_log_lines.push_back(std::move(line));
        }
    }

    Plaza2LiveSessionConfig config;
    Plaza2LiveHealthSnapshot health;
    Plaza2RuntimeProbeReport probe_report;
    private_state::Plaza2PrivateStateProjector projector;
    LiveProjectorBridge projector_bridge;
    Plaza2Env env;
    Plaza2Connection connection;
    std::vector<LiveListenerHandle> listeners;
    std::vector<Plaza2LiveStreamConfig> effective_streams;
    std::vector<std::string> operator_log_lines;
    std::optional<Plaza2Credentials> loaded_credentials;
    Plaza2Settings effective_runtime;
    std::string effective_connection_settings;
    std::string effective_connection_open_settings;
    bool started{false};
};

Plaza2LiveSessionRunner::Plaza2LiveSessionRunner(Plaza2LiveSessionConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Plaza2LiveSessionRunner::~Plaza2LiveSessionRunner() {
    if (impl_ != nullptr) {
        static_cast<void>(impl_->stop());
    }
}

Plaza2LiveSessionRunner::Plaza2LiveSessionRunner(Plaza2LiveSessionRunner&&) noexcept = default;

Plaza2LiveSessionRunner& Plaza2LiveSessionRunner::operator=(Plaza2LiveSessionRunner&&) noexcept = default;

Plaza2LiveRunResult Plaza2LiveSessionRunner::start() {
    return impl_->start();
}

Plaza2LiveRunResult Plaza2LiveSessionRunner::poll_once() {
    return impl_->poll_once();
}

Plaza2LiveRunResult Plaza2LiveSessionRunner::stop() {
    return impl_->stop();
}

const Plaza2LiveHealthSnapshot& Plaza2LiveSessionRunner::health_snapshot() const noexcept {
    return impl_->health;
}

const Plaza2RuntimeProbeReport& Plaza2LiveSessionRunner::probe_report() const noexcept {
    return impl_->probe_report;
}

const private_state::Plaza2PrivateStateProjector& Plaza2LiveSessionRunner::projector() const noexcept {
    return impl_->projector;
}

const std::vector<std::string>& Plaza2LiveSessionRunner::operator_log_lines() const noexcept {
    return impl_->operator_log_lines;
}

} // namespace moex::plaza2::cgate
