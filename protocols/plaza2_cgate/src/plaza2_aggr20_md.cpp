#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <exception>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace moex::plaza2::cgate {

namespace {

using generated::FieldCode;
using generated::StreamCode;
using generated::TableCode;

constexpr std::string_view kCredentialToken = "${MOEX_PLAZA2_TEST_CREDENTIALS}";
constexpr std::string_view kLegacyCredentialToken = "${PLAZA2_TEST_CREDENTIALS}";
constexpr std::string_view kRelativeSchemeToken = "|FILE|scheme/forts_scheme.ini|";

bool is_loopback_host(std::string_view host) {
    std::string normalized(host);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized == "127.0.0.1" || normalized == "::1" || normalized == "localhost";
}

Plaza2Error invalid_config(std::string message) {
    return {
        .code = Plaza2ErrorCode::InvalidConfiguration,
        .runtime_code = 0,
        .message = std::move(message),
    };
}

bool settings_need_credentials(std::string_view value) {
    return value.find(kCredentialToken) != std::string_view::npos ||
           value.find(kLegacyCredentialToken) != std::string_view::npos;
}

std::string substitute_credentials(std::string_view value, std::string_view credential_value) {
    std::string rendered(value);
    auto replace_all = [&](std::string_view token) {
        std::size_t position = 0;
        while ((position = rendered.find(token, position)) != std::string::npos) {
            rendered.replace(position, token.size(), credential_value);
            position += credential_value.size();
        }
    };
    replace_all(kCredentialToken);
    replace_all(kLegacyCredentialToken);
    return rendered;
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

bool contains_forbidden_public_stream(std::string_view value) {
    return value.find("FORTS_ORDLOG_REPL") != std::string_view::npos ||
           value.find("FORTS_ORDBOOK_REPL") != std::string_view::npos ||
           value.find("FORTS_DEALS_REPL") != std::string_view::npos;
}

std::optional<std::int64_t> parse_i64(std::string_view value) {
    std::int64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return parsed;
}

std::int64_t decimal_to_scaled(std::string_view value) {
    bool negative = false;
    std::int64_t whole = 0;
    std::int64_t fraction = 0;
    std::int64_t scale = 1000000;
    std::int64_t divisor = scale;
    bool after_dot = false;

    for (const char ch : value) {
        if (ch == '-' && whole == 0 && fraction == 0 && !negative) {
            negative = true;
            continue;
        }
        if (ch == '.') {
            after_dot = true;
            continue;
        }
        if (ch < '0' || ch > '9') {
            continue;
        }
        const auto digit = static_cast<std::int64_t>(ch - '0');
        if (!after_dot) {
            whole = whole * 10 + digit;
        } else if (divisor > 1) {
            divisor /= 10;
            fraction += digit * divisor;
        }
    }

    const auto scaled = whole * scale + fraction;
    return negative ? -scaled : scaled;
}

std::optional<std::int64_t> signed_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::SignedInteger) {
            return field.signed_value;
        }
        if (field.kind == Plaza2DecodedValueKind::UnsignedInteger) {
            return static_cast<std::int64_t>(field.unsigned_value);
        }
        if (field.kind == Plaza2DecodedValueKind::String || field.kind == Plaza2DecodedValueKind::Decimal) {
            return parse_i64(field.text_value);
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> unsigned_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::UnsignedInteger || field.kind == Plaza2DecodedValueKind::Timestamp) {
            return field.unsigned_value;
        }
        if (field.kind == Plaza2DecodedValueKind::SignedInteger && field.signed_value >= 0) {
            return static_cast<std::uint64_t>(field.signed_value);
        }
    }
    return std::nullopt;
}

std::string text_field(std::span<const Plaza2DecodedFieldValue> fields, FieldCode code) {
    for (const auto& field : fields) {
        if (field.field_code != code) {
            continue;
        }
        if (field.kind == Plaza2DecodedValueKind::String || field.kind == Plaza2DecodedValueKind::Decimal ||
            field.kind == Plaza2DecodedValueKind::FloatingPoint) {
            return std::string(field.text_value);
        }
        if (field.kind == Plaza2DecodedValueKind::SignedInteger) {
            return std::to_string(field.signed_value);
        }
        if (field.kind == Plaza2DecodedValueKind::UnsignedInteger) {
            return std::to_string(field.unsigned_value);
        }
    }
    return {};
}

Plaza2Error first_fatal_issue(const Plaza2RuntimeProbeReport& report) {
    for (const auto& issue : report.issues) {
        if (issue.fatal) {
            return {
                .code = Plaza2ErrorCode::ProbeIncompatible,
                .runtime_code = 0,
                .message = issue.message,
            };
        }
    }
    return {
        .code = Plaza2ErrorCode::ProbeIncompatible,
        .runtime_code = 0,
        .message = "PLAZA II runtime probe reported an incompatible TEST runtime layout",
    };
}

class Aggr20ListenerBridge final : public Plaza2ListenerEventHandler {
  public:
    explicit Aggr20ListenerBridge(Plaza2Aggr20BookProjector& projector) : projector_(projector) {}

    bool online() const noexcept {
        return online_;
    }

    bool snapshot_complete() const noexcept {
        return snapshot_complete_;
    }

    Plaza2Error on_plaza2_listener_event(const Plaza2ListenerEvent& event) override {
        try {
            return handle_event(event);
        } catch (const std::exception& error) {
            return {
                .code = Plaza2ErrorCode::CallbackFailed,
                .runtime_code = 0,
                .message = std::string("AGGR20 projector bridge failed: ") + error.what(),
            };
        } catch (...) {
            return {
                .code = Plaza2ErrorCode::CallbackFailed,
                .runtime_code = 0,
                .message = "AGGR20 projector bridge failed with an unknown exception",
            };
        }
    }

  private:
    Plaza2Error handle_event(const Plaza2ListenerEvent& event) {
        switch (event.kind) {
        case Plaza2ListenerEventKind::Open:
        case Plaza2ListenerEventKind::Timeout:
        case Plaza2ListenerEventKind::ReplState:
        case Plaza2ListenerEventKind::LifeNum:
            return {};
        case Plaza2ListenerEventKind::Close:
            online_ = false;
            return {};
        case Plaza2ListenerEventKind::TransactionBegin:
            projector_.begin_transaction();
            return {};
        case Plaza2ListenerEventKind::TransactionCommit:
            return projector_.commit();
        case Plaza2ListenerEventKind::StreamData:
            if (event.table_code != TableCode::kFortsAggrReplOrdersAggr) {
                return {};
            }
            return projector_.on_row(event.fields);
        case Plaza2ListenerEventKind::Online:
            online_ = true;
            snapshot_complete_ = true;
            return {};
        case Plaza2ListenerEventKind::ClearDeleted:
            projector_.reset();
            snapshot_complete_ = false;
            return {};
        }
        return {};
    }

    Plaza2Aggr20BookProjector& projector_;
    bool online_{false};
    bool snapshot_complete_{false};
};

} // namespace

void Plaza2Aggr20BookProjector::reset() {
    staged_rows_.clear();
    committed_ = {};
    transaction_open_ = false;
}

void Plaza2Aggr20BookProjector::begin_transaction() {
    staged_rows_.clear();
    transaction_open_ = true;
}

Plaza2Error Plaza2Aggr20BookProjector::on_row(std::span<const Plaza2DecodedFieldValue> fields) {
    if (!transaction_open_) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = 0,
            .message = "AGGR20 row arrived outside TN_BEGIN/TN_COMMIT",
        };
    }

    Plaza2Aggr20Level level;
    level.isin_id = signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrIsinId).value_or(0);
    level.price = text_field(fields, FieldCode::kFortsAggrReplOrdersAggrPrice);
    level.price_scaled = decimal_to_scaled(level.price);
    level.volume = signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrVolume).value_or(0);
    level.dir = static_cast<std::int32_t>(signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrDir).value_or(0));
    level.repl_id = unsigned_field(fields, FieldCode::kFortsAggrReplOrdersAggrReplId).value_or(0);
    level.repl_rev = signed_field(fields, FieldCode::kFortsAggrReplOrdersAggrReplRev).value_or(0);
    level.moment = unsigned_field(fields, FieldCode::kFortsAggrReplOrdersAggrMoment).value_or(0);
    level.moment_ns = unsigned_field(fields, FieldCode::kFortsAggrReplOrdersAggrMomentNs).value_or(0);
    level.synth_volume = text_field(fields, FieldCode::kFortsAggrReplOrdersAggrSynthVolume);
    staged_rows_.push_back(std::move(level));
    return {};
}

Plaza2Error Plaza2Aggr20BookProjector::commit() {
    if (!transaction_open_) {
        return {
            .code = Plaza2ErrorCode::AdapterState,
            .runtime_code = 0,
            .message = "AGGR20 TN_COMMIT arrived without TN_BEGIN",
        };
    }

    for (const auto& row : staged_rows_) {
        auto existing = std::find_if(committed_.levels.begin(), committed_.levels.end(), [&](const auto& level) {
            return level.isin_id == row.isin_id && level.dir == row.dir && level.price_scaled == row.price_scaled;
        });
        if (row.volume <= 0) {
            if (existing != committed_.levels.end()) {
                committed_.levels.erase(existing);
            }
            continue;
        }
        if (existing == committed_.levels.end()) {
            committed_.levels.push_back(row);
        } else {
            *existing = row;
        }
        committed_.last_repl_id = std::max(committed_.last_repl_id, row.repl_id);
        committed_.last_repl_rev = std::max(committed_.last_repl_rev, row.repl_rev);
    }

    committed_.row_count = committed_.levels.size();
    committed_.bid_depth_levels = 0;
    committed_.ask_depth_levels = 0;
    committed_.top_bid.reset();
    committed_.top_ask.reset();
    std::set<std::int64_t> instruments;
    for (const auto& level : committed_.levels) {
        instruments.insert(level.isin_id);
        if (level.dir == 1) {
            committed_.bid_depth_levels += 1;
            if (!committed_.top_bid.has_value() || level.price_scaled > committed_.top_bid->price_scaled) {
                committed_.top_bid = level;
            }
        } else if (level.dir == 2) {
            committed_.ask_depth_levels += 1;
            if (!committed_.top_ask.has_value() || level.price_scaled < committed_.top_ask->price_scaled) {
                committed_.top_ask = level;
            }
        }
    }
    committed_.instrument_count = instruments.size();
    staged_rows_.clear();
    transaction_open_ = false;
    return {};
}

void Plaza2Aggr20BookProjector::rollback() {
    staged_rows_.clear();
    transaction_open_ = false;
}

const Plaza2Aggr20Snapshot& Plaza2Aggr20BookProjector::snapshot() const noexcept {
    return committed_;
}

bool Plaza2Aggr20BookProjector::transaction_open() const noexcept {
    return transaction_open_;
}

std::string_view plaza2_aggr20_md_runner_state_name(Plaza2Aggr20MdRunnerState state) noexcept {
    switch (state) {
    case Plaza2Aggr20MdRunnerState::Created:
        return "Created";
    case Plaza2Aggr20MdRunnerState::Validated:
        return "Validated";
    case Plaza2Aggr20MdRunnerState::Started:
        return "Started";
    case Plaza2Aggr20MdRunnerState::Ready:
        return "Ready";
    case Plaza2Aggr20MdRunnerState::Stopped:
        return "Stopped";
    case Plaza2Aggr20MdRunnerState::Failed:
        return "Failed";
    }
    return "Unknown";
}

std::string classify_plaza2_aggr20_failure(const Plaza2Aggr20MdHealthSnapshot& health) {
    if (!health.runtime_probe_ok) {
        return "runtime_probe_failed";
    }
    if (!health.scheme_drift_ok) {
        return "schema_mismatch";
    }
    if (!health.stream_created || !health.stream_opened) {
        return "stream_open_failed";
    }
    if (!health.stream_online) {
        return "stream_not_online";
    }
    if (!health.stream_snapshot_complete) {
        return "snapshot_incomplete";
    }
    if (health.snapshot.row_count == 0) {
        return "zero_rows_observed";
    }
    return {};
}

Plaza2Error validate_plaza2_aggr20_md_config(const Plaza2Aggr20MdConfig& config) {
    if (config.profile_id.empty()) {
        return invalid_config("profile_id must be set for AGGR20 TEST bring-up");
    }
    if (config.endpoint_host.empty()) {
        return invalid_config("endpoint_host must be set for AGGR20 TEST bring-up");
    }
    if (config.runtime.environment != Plaza2Environment::Test) {
        return invalid_config("Phase 5D AGGR20 bring-up is TEST-only");
    }
    if (!config.arm_state.test_network_armed || !config.arm_state.test_session_armed ||
        !config.arm_state.test_plaza2_armed || !config.test_market_data_armed) {
        return invalid_config("AGGR20 TEST bring-up requires --armed-test-network, --armed-test-session, "
                              "--armed-test-plaza2, and --armed-test-market-data");
    }
    if (const auto runtime_error = validate_plaza2_settings(config.runtime); runtime_error) {
        return runtime_error;
    }
    if (config.runtime.env_open_settings.empty()) {
        return invalid_config("runtime.env_open_settings must be provided explicitly");
    }
    if (config.connection_settings.empty()) {
        return invalid_config("connection_settings must be provided explicitly");
    }
    if (config.stream.stream_code != StreamCode::kFortsAggrRepl) {
        return invalid_config("Phase 5D only allows FORTS_AGGR20_REPL");
    }
    if (config.stream.settings.find("FORTS_AGGR20_REPL") == std::string::npos) {
        return invalid_config("Phase 5D stream settings must explicitly use FORTS_AGGR20_REPL");
    }
    if (contains_forbidden_public_stream(config.stream.settings)) {
        return invalid_config("Phase 5D rejects FORTS_ORDLOG_REPL, FORTS_ORDBOOK_REPL, and FORTS_DEALS_REPL");
    }
    return {};
}

struct Plaza2Aggr20MdRunner::Impl {
    explicit Impl(Plaza2Aggr20MdConfig initial_config)
        : config(std::move(initial_config)), listener_bridge(projector) {}

    Plaza2Aggr20MdRunResult start() {
        if (started) {
            return fail("AGGR20 TEST runner already started");
        }

        append_operator_log("profile=" + config.profile_id);
        append_operator_log("endpoint=" + config.endpoint_host + ":" + std::to_string(config.endpoint_port));
        if (const auto validation_error = validate_plaza2_aggr20_md_config(config); validation_error) {
            return fail(validation_error.message);
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
        if (!is_loopback_host(config.endpoint_host) && !config.test_market_data_armed) {
            return fail("external AGGR20 TEST market data requires --armed-test-market-data");
        }
        if (const auto credentials = load_credentials_if_needed(); !credentials.ok) {
            return credentials;
        }

        effective_runtime = config.runtime;
        effective_runtime.env_open_settings = render_setting(config.runtime.env_open_settings);
        effective_connection_settings = render_setting(config.connection_settings);
        effective_connection_open_settings = render_setting(config.connection_open_settings);

        health.state = Plaza2Aggr20MdRunnerState::Validated;
        probe_report = Plaza2RuntimeProbe::probe(effective_runtime);
        health.compatibility = probe_report.compatibility;
        health.runtime_probe_ok = probe_report.runtime_library_loadable;
        health.scheme_drift_status = probe_report.scheme_drift.compatibility;
        health.scheme_drift_ok = probe_report.scheme_drift.compatibility == Plaza2Compatibility::Compatible ||
                                 probe_report.scheme_drift.compatibility == Plaza2Compatibility::CompatibleWithWarnings;
        health.scheme_drift_warning_count = probe_report.scheme_drift.warning_drift_count;
        health.scheme_drift_fatal_count = probe_report.scheme_drift.fatal_drift_count;
        if (probe_report.compatibility == Plaza2Compatibility::Incompatible ||
            probe_report.compatibility == Plaza2Compatibility::Unknown) {
            const auto probe_error = first_fatal_issue(probe_report);
            return fail(probe_error.message);
        }
        append_operator_log("runtime_probe=" + std::string(plaza2_compatibility_name(probe_report.compatibility)));
        append_operator_log("scheme_drift=" +
                            std::string(plaza2_compatibility_name(probe_report.scheme_drift.compatibility)));

        effective_runtime.env_open_settings =
            resolve_env_open_ini_path(effective_runtime.env_open_settings, probe_report.layout.config_dir);
        effective_stream = config.stream;
        effective_stream.settings =
            resolve_stream_scheme_path(render_setting(config.stream.settings), probe_report.layout.scheme_path);
        effective_stream.open_settings = render_setting(config.stream.open_settings);

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

        if (const auto create_error =
                listener.create(connection, effective_stream.stream_code, effective_stream.settings, &listener_bridge);
            create_error) {
            return fail(create_error.message);
        }
        health.stream_created = true;
        append_operator_log("listener=create stream=FORTS_AGGR20_REPL");

        if (const auto open_error = listener.open(effective_stream.open_settings); open_error) {
            return fail(open_error.message);
        }
        health.stream_opened = true;
        append_operator_log("listener=open stream=FORTS_AGGR20_REPL");

        started = true;
        health.state = Plaza2Aggr20MdRunnerState::Started;
        refresh_health();
        return {
            .ok = true,
            .message = "AGGR20 TEST runner started",
        };
    }

    Plaza2Aggr20MdRunResult poll_once() {
        if (!started) {
            return fail("AGGR20 TEST runner is not started");
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
            health.state = Plaza2Aggr20MdRunnerState::Ready;
        }
        return {
            .ok = true,
            .message = "AGGR20 TEST runner poll completed",
        };
    }

    Plaza2Aggr20MdRunResult stop() {
        if (!started && health.state != Plaza2Aggr20MdRunnerState::Failed) {
            health.state = Plaza2Aggr20MdRunnerState::Stopped;
            return {
                .ok = true,
                .message = "AGGR20 TEST runner already stopped",
            };
        }

        static_cast<void>(listener.close());
        static_cast<void>(listener.destroy());
        static_cast<void>(connection.close());
        static_cast<void>(connection.destroy());
        static_cast<void>(env.close());
        started = false;
        refresh_health();
        health.state = Plaza2Aggr20MdRunnerState::Stopped;
        append_operator_log("runner=stopped");
        return {
            .ok = true,
            .message = "AGGR20 TEST runner stopped",
        };
    }

    Plaza2Aggr20MdRunResult load_credentials_if_needed() {
        const bool needs_credentials = settings_need_credentials(config.runtime.env_open_settings) ||
                                       settings_need_credentials(config.connection_settings) ||
                                       settings_need_credentials(config.connection_open_settings) ||
                                       settings_need_credentials(config.stream.settings) ||
                                       settings_need_credentials(config.stream.open_settings);

        if (config.credentials.source == Plaza2CredentialSource::None) {
            if (needs_credentials) {
                return fail("AGGR20 TEST settings require ${MOEX_PLAZA2_TEST_CREDENTIALS}, but no credential source "
                            "was configured");
            }
            return {
                .ok = true,
                .message = "AGGR20 TEST credentials not required",
            };
        }

        loaded_credentials = load_plaza2_credentials(config.credentials);
        if (!loaded_credentials.has_value()) {
            return fail(config.credentials.source == Plaza2CredentialSource::Env
                            ? "AGGR20 TEST credential env var is missing or empty"
                            : "AGGR20 TEST credential file is missing or empty");
        }
        append_operator_log("credentials=" + redact_plaza2_credentials(loaded_credentials->value));
        return {
            .ok = true,
            .message = "AGGR20 TEST credentials loaded",
        };
    }

    std::string render_setting(std::string_view value) const {
        if (!loaded_credentials.has_value()) {
            return std::string(value);
        }
        return substitute_credentials(value, loaded_credentials->value);
    }

    void refresh_health() {
        health.stream_online = listener_bridge.online();
        health.stream_snapshot_complete = listener_bridge.snapshot_complete();
        health.snapshot = projector.snapshot();
        health.ready = health.runtime_probe_ok && health.scheme_drift_ok && health.stream_created &&
                       health.stream_opened && health.stream_online && health.stream_snapshot_complete &&
                       health.snapshot.row_count > 0;
        health.failure_classification = classify_plaza2_aggr20_failure(health);
    }

    Plaza2Aggr20MdRunResult fail(std::string message) {
        health.state = Plaza2Aggr20MdRunnerState::Failed;
        health.last_error = message;
        refresh_health();
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

    Plaza2Aggr20MdConfig config;
    Plaza2Aggr20MdHealthSnapshot health;
    Plaza2RuntimeProbeReport probe_report;
    Plaza2Aggr20BookProjector projector;
    Aggr20ListenerBridge listener_bridge;
    Plaza2Env env;
    Plaza2Connection connection;
    Plaza2Listener listener;
    Plaza2Aggr20MdStreamConfig effective_stream;
    std::vector<std::string> operator_log_lines;
    std::optional<Plaza2Credentials> loaded_credentials;
    Plaza2Settings effective_runtime;
    std::string effective_connection_settings;
    std::string effective_connection_open_settings;
    bool started{false};
};

Plaza2Aggr20MdRunner::Plaza2Aggr20MdRunner(Plaza2Aggr20MdConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Plaza2Aggr20MdRunner::~Plaza2Aggr20MdRunner() {
    if (impl_ != nullptr) {
        static_cast<void>(impl_->stop());
    }
}

Plaza2Aggr20MdRunner::Plaza2Aggr20MdRunner(Plaza2Aggr20MdRunner&&) noexcept = default;

Plaza2Aggr20MdRunner& Plaza2Aggr20MdRunner::operator=(Plaza2Aggr20MdRunner&&) noexcept = default;

Plaza2Aggr20MdRunResult Plaza2Aggr20MdRunner::start() {
    return impl_->start();
}

Plaza2Aggr20MdRunResult Plaza2Aggr20MdRunner::poll_once() {
    return impl_->poll_once();
}

Plaza2Aggr20MdRunResult Plaza2Aggr20MdRunner::stop() {
    return impl_->stop();
}

const Plaza2Aggr20MdHealthSnapshot& Plaza2Aggr20MdRunner::health_snapshot() const noexcept {
    return impl_->health;
}

const Plaza2RuntimeProbeReport& Plaza2Aggr20MdRunner::probe_report() const noexcept {
    return impl_->probe_report;
}

const Plaza2Aggr20BookProjector& Plaza2Aggr20MdRunner::projector() const noexcept {
    return impl_->projector;
}

const std::vector<std::string>& Plaza2Aggr20MdRunner::operator_log_lines() const noexcept {
    return impl_->operator_log_lines;
}

} // namespace moex::plaza2::cgate
