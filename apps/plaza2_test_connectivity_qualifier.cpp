#include "moex/plaza2/cgate/plaza2_trade_connectivity_qualifier.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace moex::plaza2::cgate;
using moex::plaza2::generated::StreamCode;

namespace {

struct Args {
    std::string profile_id;
    fs::path output_dir;
    std::string endpoint_host;
    std::uint16_t endpoint_port{0};
    fs::path runtime_root;
    fs::path library_path;
    fs::path scheme_dir;
    fs::path config_dir;
    std::string env_open_settings;
    std::string expected_spectra_release;
    std::string expected_scheme_sha256;
    std::string connection_settings;
    std::string connection_open_settings;
    std::vector<std::pair<std::string, std::string>> stream_settings;
    std::vector<std::pair<std::string, std::string>> stream_open_settings;
    std::string p2mqreply_settings;
    std::string p2mqreply_open_settings;
    std::string publisher_settings;
    std::string publisher_open_settings;
    std::string target_isin;
    std::string participant;
    std::int8_t account_type{2};
    std::uint32_t max_aggr20_age_ms{5000};
    std::uint32_t process_timeout_ms{50};
    std::uint32_t qualification_timeout_ms{60000};
    std::uint32_t max_polls{0};
    Plaza2CredentialSource credentials_source{Plaza2CredentialSource::None};
    std::string credentials_env_var;
    fs::path credentials_file;
    Plaza2CredentialSource software_key_source{Plaza2CredentialSource::None};
    std::string software_key_env_var;
    fs::path software_key_file;
    bool armed_test_network{false};
    bool armed_test_session{false};
    bool armed_test_plaza2{false};
    bool armed_test_market_data{false};
};

void usage() {
    std::cout << "Usage: moex_plaza2_test_connectivity_qualifier [options]\n"
                 "  --profile-id ID --output-dir DIR --endpoint-host HOST --endpoint-port PORT\n"
                 "  --runtime-root DIR --env-open-settings VALUE --connection-settings VALUE\n"
                 "  --stream-settings NAME=VALUE (repeat for required streams)\n"
                 "  --p2mqreply-settings VALUE --publisher-settings VALUE\n"
                 "  --target-isin ISIN --participant CLIENT_CODE\n"
                 "  --armed-test-network --armed-test-session --armed-test-plaza2\n"
                 "  --armed-test-market-data [--qualification-timeout-ms N] [--max-polls N]\n"
                 "  [--max-aggr20-age-ms N] [--process-timeout-ms N]\n";
}

std::optional<std::uint32_t> parse_u32(std::string_view value) {
    try {
        const auto parsed = std::stoull(std::string(value));
        if (parsed > UINT32_MAX) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::uint16_t> parse_port(std::string_view value) {
    const auto parsed = parse_u32(value);
    if (!parsed.has_value() || *parsed > UINT16_MAX) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*parsed);
}

std::optional<std::pair<std::string, std::string>> parse_assignment(std::string_view value) {
    const auto equals = value.find('=');
    if (equals == std::string_view::npos || equals == 0 || equals + 1 >= value.size()) {
        return std::nullopt;
    }
    return std::pair<std::string, std::string>{std::string(value.substr(0, equals)),
                                               std::string(value.substr(equals + 1))};
}

bool take_value(int& index, int argc, char** argv, std::string& out) {
    if (index + 1 >= argc) {
        return false;
    }
    out = argv[++index];
    return true;
}

std::optional<Args> parse_args(int argc, char** argv) {
    Args args;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        std::string value;
        if (argument == "--help") {
            usage();
            return std::nullopt;
        }
        if (argument == "--profile-id" && take_value(index, argc, argv, args.profile_id)) {
        } else if (argument == "--output-dir" && take_value(index, argc, argv, value)) {
            args.output_dir = value;
        } else if (argument == "--endpoint-host" && take_value(index, argc, argv, args.endpoint_host)) {
        } else if (argument == "--endpoint-port" && take_value(index, argc, argv, value)) {
            const auto parsed = parse_port(value);
            if (!parsed.has_value())
                return std::nullopt;
            args.endpoint_port = *parsed;
        } else if (argument == "--runtime-root" && take_value(index, argc, argv, value)) {
            args.runtime_root = value;
        } else if (argument == "--library-path" && take_value(index, argc, argv, value)) {
            args.library_path = value;
        } else if (argument == "--scheme-dir" && take_value(index, argc, argv, value)) {
            args.scheme_dir = value;
        } else if (argument == "--config-dir" && take_value(index, argc, argv, value)) {
            args.config_dir = value;
        } else if (argument == "--env-open-settings" && take_value(index, argc, argv, args.env_open_settings)) {
        } else if (argument == "--expected-spectra-release" &&
                   take_value(index, argc, argv, args.expected_spectra_release)) {
        } else if (argument == "--expected-scheme-sha256" &&
                   take_value(index, argc, argv, args.expected_scheme_sha256)) {
        } else if (argument == "--connection-settings" && take_value(index, argc, argv, args.connection_settings)) {
        } else if (argument == "--connection-open-settings" &&
                   take_value(index, argc, argv, args.connection_open_settings)) {
        } else if (argument == "--stream-settings" && take_value(index, argc, argv, value)) {
            const auto assignment = parse_assignment(value);
            if (!assignment.has_value())
                return std::nullopt;
            args.stream_settings.push_back(*assignment);
        } else if (argument == "--stream-open-settings" && take_value(index, argc, argv, value)) {
            const auto assignment = parse_assignment(value);
            if (!assignment.has_value())
                return std::nullopt;
            args.stream_open_settings.push_back(*assignment);
        } else if (argument == "--p2mqreply-settings" && take_value(index, argc, argv, args.p2mqreply_settings)) {
        } else if (argument == "--p2mqreply-open-settings" &&
                   take_value(index, argc, argv, args.p2mqreply_open_settings)) {
        } else if (argument == "--publisher-settings" && take_value(index, argc, argv, args.publisher_settings)) {
        } else if (argument == "--publisher-open-settings" &&
                   take_value(index, argc, argv, args.publisher_open_settings)) {
        } else if (argument == "--target-isin" && take_value(index, argc, argv, args.target_isin)) {
        } else if (argument == "--participant" && take_value(index, argc, argv, args.participant)) {
        } else if (argument == "--account-type" && take_value(index, argc, argv, value)) {
            const auto parsed = parse_u32(value);
            if (!parsed.has_value() || *parsed > 127)
                return std::nullopt;
            args.account_type = static_cast<std::int8_t>(*parsed);
        } else if (argument == "--max-aggr20-age-ms" && take_value(index, argc, argv, value)) {
            const auto parsed = parse_u32(value);
            if (!parsed.has_value())
                return std::nullopt;
            args.max_aggr20_age_ms = *parsed;
        } else if (argument == "--process-timeout-ms" && take_value(index, argc, argv, value)) {
            const auto parsed = parse_u32(value);
            if (!parsed.has_value())
                return std::nullopt;
            args.process_timeout_ms = *parsed;
        } else if (argument == "--qualification-timeout-ms" && take_value(index, argc, argv, value)) {
            const auto parsed = parse_u32(value);
            if (!parsed.has_value() || *parsed == 0)
                return std::nullopt;
            args.qualification_timeout_ms = *parsed;
        } else if (argument == "--max-polls" && take_value(index, argc, argv, value)) {
            const auto parsed = parse_u32(value);
            if (!parsed.has_value())
                return std::nullopt;
            args.max_polls = *parsed;
        } else if (argument == "--credentials-env-var" && take_value(index, argc, argv, args.credentials_env_var)) {
            args.credentials_source = Plaza2CredentialSource::Env;
        } else if (argument == "--credentials-file" && take_value(index, argc, argv, value)) {
            args.credentials_file = value;
            args.credentials_source = Plaza2CredentialSource::File;
        } else if (argument == "--software-key-env-var" && take_value(index, argc, argv, args.software_key_env_var)) {
            args.software_key_source = Plaza2CredentialSource::Env;
        } else if (argument == "--software-key-file" && take_value(index, argc, argv, value)) {
            args.software_key_file = value;
            args.software_key_source = Plaza2CredentialSource::File;
        } else if (argument == "--armed-test-network") {
            args.armed_test_network = true;
        } else if (argument == "--armed-test-session") {
            args.armed_test_session = true;
        } else if (argument == "--armed-test-plaza2") {
            args.armed_test_plaza2 = true;
        } else if (argument == "--armed-test-market-data") {
            args.armed_test_market_data = true;
        } else {
            std::cerr << "unknown or incomplete argument: " << argument << '\n';
            return std::nullopt;
        }
    }
    if (args.profile_id.empty() || args.output_dir.empty() || args.endpoint_host.empty() || args.runtime_root.empty() ||
        args.env_open_settings.empty() || args.connection_settings.empty() || args.target_isin.empty() ||
        args.participant.empty() || args.p2mqreply_settings.empty() || args.publisher_settings.empty()) {
        std::cerr << "required qualification arguments are missing\n";
        return std::nullopt;
    }
    return args;
}

std::string lookup(const std::vector<std::pair<std::string, std::string>>& values, std::string_view key) {
    for (const auto& [name, value] : values) {
        if (name == key)
            return value;
    }
    return {};
}

std::string lookup_open(const Args& args, std::string_view key) {
    return lookup(args.stream_open_settings, key);
}

Plaza2LiveStreamConfig typed_stream(StreamCode code, std::string label, std::string settings, const Args& args,
                                    bool explicit_override, bool require_online = true) {
    return {
        .stream_code = code,
        .label = std::move(label),
        .settings = std::move(settings),
        .open_settings =
            lookup_open(args, code == StreamCode::kFortsAggrRepl
                                  ? "FORTS_AGGR20_REPL"
                                  : std::string_view{moex::plaza2::generated::FindStreamByCode(code)->stream_name}),
        .listener_url_mode = explicit_override ? "explicit_override" : "negotiated",
        .require_online = require_online,
    };
}

Plaza2TradeConnectivityQualifierConfig make_config(const Args& args) {
    Plaza2TradeConnectivityQualifierConfig config;
    config.session.profile_id = args.profile_id;
    config.session.endpoint_host = args.endpoint_host;
    config.session.endpoint_port = args.endpoint_port;
    config.session.runtime.environment = Plaza2Environment::Test;
    config.session.runtime.runtime_root = args.runtime_root;
    config.session.runtime.library_path = args.library_path;
    config.session.runtime.scheme_dir = args.scheme_dir;
    config.session.runtime.config_dir = args.config_dir;
    config.session.runtime.env_open_settings = args.env_open_settings;
    config.session.runtime.expected_spectra_release = args.expected_spectra_release;
    config.session.runtime.expected_scheme_sha256 = args.expected_scheme_sha256;
    config.session.connection_settings = args.connection_settings;
    config.session.connection_open_settings = args.connection_open_settings;
    const auto find_setting = [&](std::string_view key) {
        const auto explicit_setting = lookup(args.stream_settings, key);
        return explicit_setting.empty() ? "p2repl://" + std::string(key) : explicit_setting;
    };
    const auto has_explicit_setting = [&](std::string_view key) { return !lookup(args.stream_settings, key).empty(); };
    const std::array required = std::to_array<std::pair<StreamCode, std::string_view>>({
        {StreamCode::kFortsTradeRepl, "FORTS_TRADE_REPL"},
        {StreamCode::kFortsUserorderbookRepl, "FORTS_USERORDERBOOK_REPL"},
        {StreamCode::kFortsPosRepl, "FORTS_POS_REPL"},
        {StreamCode::kFortsPartRepl, "FORTS_PART_REPL"},
        {StreamCode::kFortsRefdataRepl, "FORTS_REFDATA_REPL"},
        {StreamCode::kFortsSessionstateRepl, "FORTS_SESSIONSTATE_REPL"},
        {StreamCode::kFortsInstrumentstateRepl, "FORTS_INSTRUMENTSTATE_REPL"},
    });
    for (const auto& [code, label] : required) {
        config.session.streams.push_back(
            typed_stream(code, std::string(label), find_setting(label), args, has_explicit_setting(label)));
    }
    config.session.streams.push_back(typed_stream(StreamCode::kFortsAggrRepl, "FORTS_AGGR_REPL",
                                                  find_setting("FORTS_AGGR20_REPL"), args,
                                                  has_explicit_setting("FORTS_AGGR20_REPL"), false));
    config.session.streams.push_back({
        .stream_code = kNoStreamCode,
        .label = "p2mqreply",
        .settings = args.p2mqreply_settings,
        .open_settings = args.p2mqreply_open_settings,
        .listener_url_mode = "explicit_override",
        .require_online = false,
    });
    config.session.credentials.source = args.credentials_source;
    config.session.credentials.env_var = args.credentials_env_var;
    config.session.credentials.file_path = args.credentials_file;
    config.session.software_key.source = args.software_key_source;
    config.session.software_key.env_var = args.software_key_env_var;
    config.session.software_key.file_path = args.software_key_file;
    config.session.arm_state.test_network_armed = args.armed_test_network;
    config.session.arm_state.test_session_armed = args.armed_test_session;
    config.session.arm_state.test_plaza2_armed = args.armed_test_plaza2;
    config.session.process_timeout_ms = args.process_timeout_ms;
    config.session.open_publisher = true;
    config.session.publisher_settings = args.publisher_settings;
    config.session.publisher_open_settings = args.publisher_open_settings;
    config.target.isin = args.target_isin;
    config.target.participant = args.participant;
    config.target.expected_position_account_type = args.account_type;
    config.test_market_data_armed = args.armed_test_market_data;
    config.max_aggr20_age_ms = args.max_aggr20_age_ms;
    return config;
}

std::string json_escape(std::string_view value) {
    std::string out;
    for (const char ch : value) {
        if (ch == '\\')
            out += "\\\\";
        else if (ch == '"')
            out += "\\\"";
        else if (ch == '\n')
            out += "\\n";
        else
            out.push_back(ch);
    }
    return out;
}

std::string timestamp_utc() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::array<char, 32> buffer{};
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer.data();
}

std::string_view build_os() {
#if defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(_WIN32)
    return "Windows";
#else
    return "unknown";
#endif
}

std::string_view build_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

std::string host_hash() {
    std::array<char, 256> buffer{};
    if (gethostname(buffer.data(), buffer.size() - 1) != 0)
        return "unavailable";
    return plaza2_sha256_hex(std::string_view(buffer.data())).substr(0, 16);
}

void write_receipt(const fs::path& path, const Args& args, const Plaza2TradeConnectivityQualifier& qualifier,
                   std::string_view git_sha, std::string_view result_message) {
    const auto& snapshot = qualifier.qualification();
    const auto& probe = qualifier.private_session().probe_report();
    const auto& health = qualifier.private_session().health_snapshot();
    std::string json;
    json += "{\n";
    const auto field = [&](std::string_view key, std::string_view value, bool comma = true) {
        json += "  \"" + std::string(key) + "\": \"" + json_escape(value) + "\"" + (comma ? ",\n" : "\n");
    };
    const auto boolean = [&](std::string_view key, bool value, bool comma = true) {
        json += "  \"" + std::string(key) + "\": " + (value ? "true" : "false") + (comma ? ",\n" : "\n");
    };
    const auto number = [&](std::string_view key, auto value, bool comma = true) {
        json += "  \"" + std::string(key) + "\": " + std::to_string(value) + (comma ? ",\n" : "\n");
    };
    const auto string_array = [&](std::string_view key, const auto& values) {
        json += "  \"" + std::string(key) + "\": [";
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0)
                json += ", ";
            json += "\"" + json_escape(values[index]) + "\"";
        }
        json += "],\n";
    };
    field("schema", "plaza2_test_connectivity_receipt.v1");
    field("git_sha", git_sha);
    field("timestamp_utc", timestamp_utc());
    field("hostname_hash", host_hash());
    field("os", build_os());
    field("arch", build_arch());
    field("profile_id", args.profile_id);
    field("endpoint_host", args.endpoint_host);
    number("endpoint_port", args.endpoint_port);
    field("runtime_library_sha256", probe.runtime_library_sha256);
    field("runtime_library_path", probe.layout.library_path.string());
    field("runtime_root", probe.layout.runtime_root.string());
    field("scheme_path", probe.layout.scheme_path.string());
    field("config_dir", probe.layout.config_dir.string());
    field("spectra_release", probe.layout.version_markers.spectra_release);
    field("dds_version", probe.layout.version_markers.dds_version);
    field("target_polygon", probe.layout.version_markers.target_polygon);
    field("scheme_sha256", probe.scheme_drift.runtime_scheme_sha256);
    field("runtime_compatibility", plaza2_compatibility_name(probe.compatibility));
    field("runtime_version_observed",
          probe.layout.version_markers.spectra_release + "/" + probe.layout.version_markers.dds_version);
    boolean("runtime_library_loadable", probe.runtime_library_loadable);
    boolean("runtime_trading_capable", snapshot.runtime_trading_capable);
    boolean("abi_subset_compatible", probe.runtime_library_loadable);
    boolean("scheme_subset_compatible", probe.scheme_drift.compatibility != Plaza2Compatibility::Incompatible);
    boolean("full_version_certified", false);
    string_array("installed_config_files", probe.layout.present_config_files);
    string_array("expected_config_files", probe.layout.expected_config_files);
    string_array("resolved_symbols", probe.resolved_symbols);
    std::vector<std::string> missing_runtime_symbols;
    for (const auto required : Plaza2RuntimeProbe::required_runtime_symbols()) {
        if (std::ranges::find(probe.resolved_symbols, required) == probe.resolved_symbols.end()) {
            missing_runtime_symbols.emplace_back(required);
        }
    }
    string_array("missing_runtime_symbols", missing_runtime_symbols);
    string_array("missing_trading_symbols", probe.missing_trading_symbols);
    std::vector<std::string> dbscheme_aliases;
    for (const auto& stream : health.streams) {
        dbscheme_aliases.push_back(stream.stream_name);
    }
    string_array("configured_dbscheme_aliases", dbscheme_aliases);
    number("scheme_fatal_drift_count", probe.scheme_drift.fatal_drift_count);
    number("scheme_warning_drift_count", probe.scheme_drift.warning_drift_count);
    field("target_isin", args.target_isin);
    field("participant", args.participant);
    boolean("target_found", snapshot.target_found);
    number("target_isin_id", snapshot.target_isin_id);
    number("target_sess_id", snapshot.target_sess_id);
    boolean("target_in_fut_sess_contents", snapshot.target_current_session_member);
    boolean("target_session_status_available", snapshot.target_session_status_available);
    number("target_session_status", snapshot.target_session_status);
    boolean("target_session_add_capable", snapshot.target_session_add_capable);
    boolean("target_instrument_status_available", snapshot.target_instrument_status_available);
    number("target_instrument_status", snapshot.target_instrument_status);
    boolean("target_instrument_add_capable", snapshot.target_instrument_add_capable);
    field("target_min_step", snapshot.target_min_step);
    number("target_trade_mode_id", snapshot.target_trade_mode_id);
    boolean("target_refdata_present", snapshot.target_refdata_present);
    boolean("target_aggr20_two_sided", snapshot.target_aggr20_two_sided);
    number("target_aggr20_age_ms", snapshot.target_aggr20_age_ms);
    number("target_aggr20_repl_id", snapshot.target_aggr20_repl_id);
    number("target_aggr20_repl_rev", snapshot.target_aggr20_repl_rev);
    boolean("private_streams_ready", snapshot.private_streams_ready);
    boolean("status_streams_ready", snapshot.status_streams_ready);
    boolean("p2mqreply_open", snapshot.p2mqreply_open);
    boolean("publisher_open", snapshot.publisher_open);
    boolean("participant_limit_unique", snapshot.participant_limit_unique);
    boolean("participant_limits_set", snapshot.participant_limits_set);
    number("applicable_position_count", snapshot.applicable_position_count);
    number("position_account_type", snapshot.position_account_type);
    number("position_xpos", snapshot.position_xpos);
    boolean("position_identity_exact", snapshot.position_identity_exact);
    json += "  \"position_census\": [";
    bool first_position = true;
    for (const auto& position : qualifier.private_session().projector().positions()) {
        if (position.scope != moex::plaza2::private_state::PositionScope::kClient ||
            position.account_code != args.participant) {
            continue;
        }
        if (!first_position) {
            json += ", ";
        }
        first_position = false;
        json += "{\"isin_id\":" + std::to_string(position.isin_id) +
                ",\"account_type\":" + std::to_string(position.account_type) +
                ",\"xpos\":" + std::to_string(position.xpos) + "}";
    }
    json += "],\n";
    boolean("connectivity_ready", snapshot.connectivity_ready);
    boolean("market_state_ready", snapshot.market_state_ready);
    boolean("account_state_ready", snapshot.account_state_ready);
    boolean("publisher_ready", snapshot.publisher_ready);
    boolean("add_order_qualified", snapshot.add_order_qualified);
    number("last_error_code", static_cast<std::uint16_t>(health.last_error_code));
    number("last_error_runtime_code", health.last_error_runtime_code);
    field("last_error", health.last_error);
    field("failing_listener", health.failing_listener);
    json += "  \"stream_status\": [";
    for (std::size_t index = 0; index < health.streams.size(); ++index) {
        if (index != 0)
            json += ", ";
        const auto& stream = health.streams[index];
        json += "{\"name\":\"" + json_escape(stream.stream_name) + "\",\"listener_url_mode\":\"" +
                json_escape(stream.listener_url_mode) + "\",\"created\":" + (stream.created ? "true" : "false") +
                ",\"opened\":" + (stream.opened ? "true" : "false") +
                ",\"online\":" + (stream.online ? "true" : "false") +
                ",\"snapshot_complete\":" + (stream.snapshot_complete ? "true" : "false") +
                ",\"required_online\":" + (stream.required_online ? "true" : "false") + "}";
    }
    json += "],\n";
    field("qualification_state", plaza2_qualification_terminal_name(snapshot.terminal));
    field("lifecycle_state", plaza2_qualification_state_name(snapshot.state));
    field("terminal_classification", plaza2_qualification_terminal_name(snapshot.terminal));
    field("result_message", result_message);
    json += "  \"failure_reasons\": [";
    for (std::size_t index = 0; index < snapshot.failure_reasons.size(); ++index) {
        if (index != 0)
            json += ", ";
        json += "\"" + json_escape(snapshot.failure_reasons[index]) + "\"";
    }
    json += "]\n}\n";
    std::ofstream output(path);
    output << json;
    const auto digest = plaza2_sha256_hex(json);
    std::ofstream hash(path.string() + ".sha256");
    hash << digest << "  " << path.filename().string() << '\n';
    std::cout << "receipt_sha256=" << digest << '\n';
}

void write_review(const fs::path& path, const Args& args, const Plaza2TradeConnectivityQualifier& qualifier,
                  std::string_view git_sha, std::string_view receipt_sha, std::string_view result_message) {
    const auto& snapshot = qualifier.qualification();
    std::ofstream output(path);
    output << "# PLAZA II TEST connectivity qualification V1\n\n"
              "This receipt is read-only qualification evidence. The qualifier has no publisher-message send path.\n\n"
           << "- Git SHA: `" << git_sha << "`\n"
           << "- Target: `" << args.target_isin << "`\n"
           << "- Participant: `" << args.participant << "`\n"
           << "- Result: `" << result_message << "`\n"
           << "- Terminal classification: `" << plaza2_qualification_terminal_name(snapshot.terminal) << "`\n"
           << "- Receipt SHA-256: `" << receipt_sha
           << "`\n\n"
              "## Readiness\n\n"
           << "- connectivity_ready: `" << (snapshot.connectivity_ready ? "true" : "false") << "`\n"
           << "- market_state_ready: `" << (snapshot.market_state_ready ? "true" : "false") << "`\n"
           << "- account_state_ready: `" << (snapshot.account_state_ready ? "true" : "false") << "`\n"
           << "- publisher_ready: `" << (snapshot.publisher_ready ? "true" : "false") << "`\n"
           << "- add_order_qualified (informational only): `" << (snapshot.add_order_qualified ? "true" : "false")
           << "`\n\n"
              "A qualification failure is terminal for this run and never issues cleanup commands. Full-version\n"
              "certification remains false unless a broader ABI and scheme review is completed.\n";
}

} // namespace

int main(int argc, char** argv) {
    const auto parsed = parse_args(argc, argv);
    if (!parsed.has_value())
        return 1;
    const auto args = *parsed;
    std::filesystem::create_directories(args.output_dir);

    Plaza2TradeConnectivityQualifier qualifier(make_config(args));
    auto result = qualifier.start();
    if (result.ok) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(args.qualification_timeout_ms);
        std::uint32_t polls = 0;
        while (!qualifier.qualification().add_order_qualified && std::chrono::steady_clock::now() < deadline &&
               (args.max_polls == 0 || polls < args.max_polls)) {
            result = qualifier.poll_once();
            if (!result.ok)
                break;
            ++polls;
        }
        if (result.ok && !qualifier.qualification().add_order_qualified) {
            result = {.ok = false,
                      .message = std::chrono::steady_clock::now() >= deadline
                                     ? "qualification did not reach all readiness gates before monotonic timeout"
                                     : "qualification did not reach all readiness gates before max polls"};
        }
    }

#ifdef MOEX_SOURCE_GIT_SHA
    constexpr std::string_view git_sha = MOEX_SOURCE_GIT_SHA;
#else
    const auto git_sha = std::getenv("MOEX_GIT_SHA") == nullptr ? "unknown" : std::getenv("MOEX_GIT_SHA");
#endif
    const auto receipt_path = args.output_dir / "plaza2_test_connectivity_receipt.json";
    write_receipt(receipt_path, args, qualifier, git_sha, result.message);
    const auto receipt_text = [&]() {
        std::ifstream input(receipt_path);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    }();
    const auto receipt_sha = plaza2_sha256_hex(receipt_text);
    write_review(args.output_dir / "plaza2_test_connectivity_qualification_v1_review.md", args, qualifier, git_sha,
                 receipt_sha, result.message);
    static_cast<void>(qualifier.stop());
    return result.ok ? 0 : 1;
}
