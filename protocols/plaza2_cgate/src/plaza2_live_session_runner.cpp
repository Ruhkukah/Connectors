#include "moex/plaza2/cgate/plaza2_live_session_runner.hpp"
#include "moex/plaza2/cgate/plaza2_private_state_bridge.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moex::plaza2::cgate {

namespace {

using generated::StreamCode;

constexpr std::string_view kCredentialToken = "${MOEX_PLAZA2_TEST_CREDENTIALS}";
constexpr std::string_view kLegacyCredentialToken = "${PLAZA2_TEST_CREDENTIALS}";
constexpr std::string_view kSoftwareKeyToken = "${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}";
constexpr std::string_view kRelativeSchemeToken = "|FILE|scheme/forts_scheme.ini|";
constexpr std::array<StreamCode, 5> kRequiredPrivateStreams = {
    StreamCode::kFortsTradeRepl, StreamCode::kFortsUserorderbookRepl, StreamCode::kFortsPosRepl,
    StreamCode::kFortsPartRepl,  StreamCode::kFortsRefdataRepl,
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

std::string stream_label(const Plaza2LiveStreamConfig& stream) {
    if (!stream.label.empty()) {
        return stream.label;
    }
    return stream_name(stream.stream_code);
}

bool settings_need_credentials(std::string_view value) {
    return value.find(kCredentialToken) != std::string_view::npos ||
           value.find(kLegacyCredentialToken) != std::string_view::npos;
}

bool settings_need_software_key(std::string_view value) {
    return value.find(kSoftwareKeyToken) != std::string_view::npos;
}

void replace_all(std::string& rendered, std::string_view token, std::string_view replacement) {
    std::size_t position = 0;
    while ((position = rendered.find(token, position)) != std::string::npos) {
        rendered.replace(position, token.size(), replacement);
        position += replacement.size();
    }
}

std::string resolve_stream_scheme_path(std::string_view value, const std::filesystem::path& scheme_path) {
    std::string rendered(value);
    const auto replacement = std::string("|FILE|") + scheme_path.string() + "|";
    std::size_t position = 0;
    while ((position = rendered.find(kRelativeSchemeToken, position)) != std::string::npos) {
        rendered.replace(position, kRelativeSchemeToken.size(), replacement);
        position += replacement.size();
    }
    return rendered;
}

std::string resolve_env_open_ini_path(std::string_view value, const std::filesystem::path& config_dir) {
    std::string rendered(value);
    constexpr std::string_view kPrefix = "ini=";
    const auto begin = rendered.find(kPrefix);
    if (begin == std::string::npos) {
        return rendered;
    }
    const auto value_begin = begin + kPrefix.size();
    const auto value_end = rendered.find(';', value_begin);
    const auto value_size = (value_end == std::string::npos ? rendered.size() : value_end) - value_begin;
    const auto raw_path = std::filesystem::path(rendered.substr(value_begin, value_size));
    if (raw_path.is_absolute()) {
        return rendered;
    }
    auto resolved = config_dir / raw_path;
    if (!std::filesystem::exists(resolved) && raw_path.has_parent_path() && raw_path.begin() != raw_path.end() &&
        *raw_path.begin() == "config") {
        resolved = config_dir / raw_path.filename();
    }
    rendered.replace(value_begin, value_size, resolved.string());
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

using LiveProjectorBridge = Plaza2PrivateStateBridge;
class HealthTrackingHandler final : public Plaza2ListenerEventHandler {
  public:
    HealthTrackingHandler(Plaza2LiveHealthSnapshot& health, generated::StreamCode stream_code,
                          Plaza2ListenerEventHandler* delegate)
        : health_(health), stream_code_(stream_code), delegate_(delegate) {}

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        for (auto& stream : health_.streams) {
            if (stream.stream_code != stream_code_) {
                continue;
            }
            switch (event.kind) {
            case Plaza2ListenerEventKind::Close:
                stream.online = false;
                stream.snapshot_complete = false;
                break;
            case Plaza2ListenerEventKind::LifeNum:
                if (has_lifenum_ && last_lifenum_ != event.unsigned_value) {
                    stream.online = false;
                    stream.snapshot_complete = false;
                }
                has_lifenum_ = true;
                last_lifenum_ = event.unsigned_value;
                break;
            case Plaza2ListenerEventKind::Online:
                stream.online = true;
                stream.snapshot_complete = true;
                break;
            default:
                break;
            }
            break;
        }
        return delegate_ == nullptr ? Plaza2Error{} : delegate_->on_plaza2_listener_event(event);
    }

  private:
    Plaza2LiveHealthSnapshot& health_;
    generated::StreamCode stream_code_;
    Plaza2ListenerEventHandler* delegate_;
    bool has_lifenum_{false};
    std::uint64_t last_lifenum_{0};
};

struct LiveListenerHandle {
    Plaza2LiveStreamConfig config;
    std::unique_ptr<HealthTrackingHandler> health_handler;
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
                .stream_name = stream_label(stream),
                .listener_url_mode = stream.listener_url_mode,
                .required_online = stream.require_online,
            });
        }
    }

    Plaza2LiveRunResult start() {
        if (started) {
            return fail("PLAZA II live TEST runner already started");
        }

        health.failing_listener.clear();

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

        if (const auto secrets = load_secrets_if_needed(); !secrets.ok) {
            return secrets;
        }

        effective_runtime = config.runtime;
        effective_runtime.env_open_settings = render_setting(config.runtime.env_open_settings);
        effective_connection_settings = render_setting(config.connection_settings);
        effective_connection_open_settings = render_setting(config.connection_open_settings);

        health.state = Plaza2LiveRunnerState::Validated;
        probe_report = Plaza2RuntimeProbe::probe(effective_runtime);
        health.compatibility = probe_report.compatibility;
        health.runtime_probe_ok = probe_report.runtime_library_loadable;
        health.scheme_drift_status = probe_report.scheme_drift.compatibility;
        health.scheme_drift_ok = probe_report.scheme_drift.compatibility == Plaza2Compatibility::Compatible ||
                                 probe_report.scheme_drift.compatibility == Plaza2Compatibility::CompatibleWithWarnings;
        health.scheme_drift_warning_count = probe_report.scheme_drift.warning_drift_count;
        health.scheme_drift_fatal_count = probe_report.scheme_drift.fatal_drift_count;
        health.scheme_drift_warning_tables = probe_report.scheme_drift.warning_drift_tables;
        health.scheme_drift_fatal_tables = probe_report.scheme_drift.fatal_drift_tables;
        health.last_scheme_drift_warning = probe_report.scheme_drift.last_warning_drift_reason;
        health.last_scheme_drift_fatal = probe_report.scheme_drift.last_fatal_drift_reason;
        if (probe_report.compatibility == Plaza2Compatibility::Incompatible ||
            probe_report.compatibility == Plaza2Compatibility::Unknown) {
            return fail(first_fatal_issue_message(probe_report));
        }
        append_operator_log("runtime_probe=" + std::string(plaza2_compatibility_name(probe_report.compatibility)));
        append_operator_log("scheme_drift=" +
                            std::string(plaza2_compatibility_name(probe_report.scheme_drift.compatibility)));
        append_operator_log("scheme_drift_warning_count=" + std::to_string(health.scheme_drift_warning_count));
        append_operator_log("scheme_drift_fatal_count=" + std::to_string(health.scheme_drift_fatal_count));

        effective_runtime.env_open_settings =
            resolve_env_open_ini_path(effective_runtime.env_open_settings, probe_report.layout.config_dir);

        effective_streams.clear();
        effective_streams.reserve(config.streams.size());
        for (const auto& stream : config.streams) {
            effective_streams.push_back({
                .stream_code = stream.stream_code,
                .label = stream.label,
                .settings =
                    resolve_stream_scheme_path(render_setting(stream.settings), probe_report.layout.scheme_path),
                .open_settings = render_setting(stream.open_settings),
                .listener_url_mode = stream.listener_url_mode,
                .require_online = stream.require_online,
                .handler = stream.handler,
            });
        }

        bridge_stream_codes.clear();
        bridge_stream_codes.reserve(effective_streams.size());
        for (const auto& stream : effective_streams) {
            bridge_stream_codes.push_back(stream.stream_code);
        }
        if (const auto bridge_reset = projector_bridge.reset(bridge_stream_codes); bridge_reset) {
            return fail(bridge_reset.message);
        }
        if (const auto bridge_open = projector_bridge.begin_run(); bridge_open) {
            return fail(bridge_open.message);
        }

        if (const auto env_error = env.open(effective_runtime); env_error) {
            health.failing_listener = "environment";
            return fail_error(env_error);
        }
        append_operator_log("env=open");

        if (const auto connection_error = connection.create(env, effective_connection_settings); connection_error) {
            health.failing_listener = "connection";
            return fail_error(connection_error);
        }
        append_operator_log("connection=create");

        if (const auto open_error = connection.open(effective_connection_open_settings); open_error) {
            health.failing_listener = "connection";
            return fail_error(open_error);
        }
        append_operator_log("connection=open");

        if (config.open_publisher) {
            health.failing_listener = "publisher";
            if (config.publisher_settings.empty()) {
                return fail("publisher_settings must be provided when open_publisher is enabled");
            }
            if (const auto create_error = publisher.create(connection, render_setting(config.publisher_settings));
                create_error) {
                return fail_error(create_error);
            }
            health.publisher_created = true;
            append_operator_log("publisher=create");
            if (const auto open_error = publisher.open(render_setting(config.publisher_open_settings)); open_error) {
                return fail_error(open_error);
            }
            health.publisher_opened = true;
            append_operator_log("publisher=open");
        }

        listeners.clear();
        listeners.reserve(effective_streams.size());
        for (const auto& stream : effective_streams) {
            health.failing_listener = stream_label(stream);
            LiveListenerHandle handle{
                .config = stream,
            };
            if (stream.stream_code != kNoStreamCode) {
                handle.health_handler = std::make_unique<HealthTrackingHandler>(
                    health, stream.stream_code, stream.handler == nullptr ? &projector_bridge : stream.handler);
            }
            const auto create_error = stream.stream_code == kNoStreamCode
                                          ? handle.listener.create(connection, stream.settings)
                                          : handle.listener.create(connection, stream.stream_code, stream.settings,
                                                                   handle.health_handler.get());
            if (create_error) {
                return fail_error(create_error);
            }
            mark_stream_created(stream.stream_code);
            append_operator_log("listener=create stream=" + stream_label(stream));

            if (const auto open_error = handle.listener.open(stream.open_settings); open_error) {
                return fail_error(open_error);
            }
            mark_stream_opened(stream.stream_code);
            append_operator_log("listener=open stream=" + stream_label(stream));
            listeners.push_back(std::move(handle));
        }

        started = true;
        health.failing_listener.clear();
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
            return fail_error(process_error);
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
        if (health.publisher_created) {
            static_cast<void>(publisher.close());
            static_cast<void>(publisher.destroy());
            health.publisher_created = false;
            health.publisher_opened = false;
        }
        static_cast<void>(connection.close());
        static_cast<void>(connection.destroy());
        static_cast<void>(env.close());
        static_cast<void>(projector_bridge.end_run());
        for (auto& stream : health.streams) {
            stream.online = false;
            stream.snapshot_complete = false;
        }
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
        if (config.runtime.env_open_settings.find(kCredentialToken) != std::string::npos ||
            config.runtime.env_open_settings.find(kLegacyCredentialToken) != std::string::npos) {
            return fail("PLAZA II TEST env_open_settings must use ${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}, not the "
                        "exchange credential variable");
        }
        if (config.connection_settings.empty()) {
            return fail("connection_settings must be provided explicitly");
        }
        std::vector<StreamCode> present_streams;
        present_streams.reserve(config.streams.size());
        for (const auto& stream : config.streams) {
            if (stream.settings.empty()) {
                return fail("every configured PLAZA II listener must provide settings");
            }
            if (stream.stream_code != kNoStreamCode) {
                present_streams.push_back(stream.stream_code);
            }
        }
        std::sort(present_streams.begin(), present_streams.end());
        if (std::adjacent_find(present_streams.begin(), present_streams.end()) != present_streams.end()) {
            return fail("PLAZA II listener stream codes must be unique");
        }

        auto expected_streams = std::vector<StreamCode>(kRequiredPrivateStreams.begin(), kRequiredPrivateStreams.end());
        std::sort(expected_streams.begin(), expected_streams.end());
        if (!std::includes(present_streams.begin(), present_streams.end(), expected_streams.begin(),
                           expected_streams.end())) {
            return fail("PLAZA II listener set must include FORTS_TRADE_REPL, FORTS_USERORDERBOOK_REPL, "
                        "FORTS_POS_REPL, FORTS_PART_REPL, and FORTS_REFDATA_REPL");
        }
        if (config.open_publisher && config.publisher_settings.empty()) {
            return fail("publisher_settings must be provided when open_publisher is enabled");
        }
        for (const auto& stream : config.streams) {
            if (stream.stream_code == kNoStreamCode && stream.require_online) {
                return fail("untyped PLAZA II listeners must not require replication ONLINE");
            }
        }
        return {
            .ok = true,
            .message = "PLAZA II TEST config validated",
        };
    }

    Plaza2LiveRunResult load_secrets_if_needed() {
        const bool needs_credentials =
            settings_need_credentials(config.runtime.env_open_settings) ||
            settings_need_credentials(config.connection_settings) ||
            settings_need_credentials(config.connection_open_settings) ||
            std::any_of(config.streams.begin(), config.streams.end(), [](const Plaza2LiveStreamConfig& stream) {
                return settings_need_credentials(stream.settings) || settings_need_credentials(stream.open_settings);
            });
        const bool needs_software_key =
            settings_need_software_key(config.runtime.env_open_settings) ||
            settings_need_software_key(config.connection_settings) ||
            settings_need_software_key(config.connection_open_settings) ||
            std::any_of(config.streams.begin(), config.streams.end(), [](const Plaza2LiveStreamConfig& stream) {
                return settings_need_software_key(stream.settings) || settings_need_software_key(stream.open_settings);
            });

        if (config.credentials.source == Plaza2CredentialSource::None) {
            if (needs_credentials) {
                return fail(
                    "PLAZA II TEST settings require ${MOEX_PLAZA2_TEST_CREDENTIALS}, but no credential source was "
                    "configured");
            }
        } else {
            loaded_credentials = load_plaza2_credentials(config.credentials);
            if (!loaded_credentials.has_value()) {
                return fail(config.credentials.source == Plaza2CredentialSource::Env
                                ? "PLAZA II TEST credential env var is missing or empty"
                                : "PLAZA II TEST credential file is missing or empty");
            }

            append_operator_log("credentials_source=" +
                                std::string(config.credentials.source == Plaza2CredentialSource::Env ? "env" : "file"));
            append_operator_log("credentials=" + redact_plaza2_credentials(loaded_credentials->value));
        }

        if (config.software_key.source == Plaza2CredentialSource::None) {
            if (needs_software_key) {
                return fail("PLAZA II TEST settings require ${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}, but no software-key "
                            "source was configured");
            }
            return {
                .ok = true,
                .message = "PLAZA II TEST secrets loaded",
            };
        }

        loaded_software_key = load_plaza2_credentials(config.software_key);
        if (!loaded_software_key.has_value()) {
            return fail(config.software_key.source == Plaza2CredentialSource::Env
                            ? "PLAZA II TEST software-key env var is missing or empty"
                            : "PLAZA II TEST software-key file is missing or empty");
        }

        append_operator_log("software_key_source=" +
                            std::string(config.software_key.source == Plaza2CredentialSource::Env ? "env" : "file"));
        append_operator_log("software_key=" + redact_plaza2_credentials(loaded_software_key->value));
        return {
            .ok = true,
            .message = "PLAZA II TEST secrets loaded",
        };
    }

    std::string render_setting(std::string_view value) const {
        std::string rendered(value);
        if (loaded_credentials.has_value()) {
            replace_all(rendered, kCredentialToken, loaded_credentials->value);
            replace_all(rendered, kLegacyCredentialToken, loaded_credentials->value);
        }
        if (loaded_software_key.has_value()) {
            replace_all(rendered, kSoftwareKeyToken, loaded_software_key->value);
        }
        return rendered;
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

        const auto projected_streams = projector.stream_health();
        for (auto& status : health.streams) {
            status.periodic_snapshot_consistent = false;
            for (const auto& projected : projected_streams) {
                if (projected.stream_code == status.stream_code) {
                    status.periodic_snapshot_consistent = projected.periodic_snapshot_consistent;
                    break;
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
        health.ready =
            health.runtime_probe_ok && health.scheme_drift_ok && (!config.open_publisher || health.publisher_opened) &&
            std::all_of(health.streams.begin(), health.streams.end(), [](const Plaza2LiveStreamStatus& stream) {
                const bool initial_ready = !stream.required_online || (stream.online && stream.snapshot_complete);
                const bool periodic_ready = stream.stream_code != generated::StreamCode::kFortsUserorderbookRepl ||
                                            stream.periodic_snapshot_consistent;
                return stream.created && stream.opened && initial_ready && periodic_ready;
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

    Plaza2LiveRunResult fail_error(const Plaza2Error& error) {
        health.last_error_code = error.code;
        health.last_error_runtime_code = error.runtime_code;
        return fail(error.message);
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
    Plaza2Publisher publisher;
    std::vector<LiveListenerHandle> listeners;
    std::vector<Plaza2LiveStreamConfig> effective_streams;
    std::vector<generated::StreamCode> bridge_stream_codes;
    std::vector<std::string> operator_log_lines;
    std::optional<Plaza2Credentials> loaded_credentials;
    std::optional<Plaza2Credentials> loaded_software_key;
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
