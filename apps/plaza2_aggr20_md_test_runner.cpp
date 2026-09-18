#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using moex::plaza2::cgate::plaza2_aggr20_md_runner_state_name;
using moex::plaza2::cgate::plaza2_compatibility_name;
using moex::plaza2::cgate::Plaza2Aggr20Level;
using moex::plaza2::cgate::Plaza2Aggr20MdConfig;
using moex::plaza2::cgate::Plaza2Aggr20MdRunner;
using moex::plaza2::cgate::Plaza2Aggr20MdRunnerState;
using moex::plaza2::cgate::Plaza2ClockEvidence;
using moex::plaza2::cgate::Plaza2CredentialSource;
using moex::plaza2::cgate::Plaza2Environment;

struct RunnerArgs {
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
    std::string expected_runtime_library_sha256;
    std::string expected_scheme_sha256;
    std::string connection_settings;
    std::string connection_open_settings;
    std::string stream_settings;
    std::string stream_open_settings;
    std::uint32_t process_timeout_ms{50};
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
    fs::path clock_evidence_file;
    std::uint32_t max_polls{8};
};

std::string escape_json(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const auto ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    return out;
}

std::optional<std::uint16_t> parse_port(std::string_view value) {
    try {
        const auto parsed = std::stoul(std::string(value));
        if (parsed > 65535U) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::uint32_t> parse_u32(std::string_view value) {
    try {
        return static_cast<std::uint32_t>(std::stoul(std::string(value)));
    } catch (...) {
        return std::nullopt;
    }
}

std::string trim_copy(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0)
        ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
        --last;
    return std::string(value.substr(first, last - first));
}

std::optional<std::int64_t> parse_i64(std::string_view value) {
    if (value.empty())
        return std::nullopt;
    std::int64_t parsed = 0;
    const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || pointer != value.data() + value.size())
        return std::nullopt;
    return parsed;
}

std::optional<std::uint64_t> parse_u64(std::string_view value) {
    if (value.empty() || value.front() == '-')
        return std::nullopt;
    std::uint64_t parsed = 0;
    const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || pointer != value.data() + value.size())
        return std::nullopt;
    return parsed;
}

std::optional<bool> parse_bool(std::string_view value) {
    if (value == "true")
        return true;
    if (value == "false")
        return false;
    return std::nullopt;
}

struct ClockEvidenceLoadResult {
    std::optional<Plaza2ClockEvidence> evidence;
    std::string error;
};

ClockEvidenceLoadResult read_clock_evidence(const fs::path& path) {
    if (path.empty()) {
        return {.error = "--clock-evidence-file is required; certification launch has no current clock evidence"};
    }

    std::ifstream input(path);
    if (!input) {
        return {.error = "clock evidence file could not be opened: " + path.string()};
    }

    Plaza2ClockEvidence evidence;
    std::array<bool, 10> seen{};
    constexpr std::size_t kSyncSource = 0;
    constexpr std::size_t kSyncStatus = 1;
    constexpr std::size_t kWallOffset = 2;
    constexpr std::size_t kOffsetUncertainty = 3;
    constexpr std::size_t kClockId = 4;
    constexpr std::size_t kCurrentWall = 5;
    constexpr std::size_t kCurrentMonotonic = 6;
    constexpr std::size_t kSyncMonotonic = 7;
    constexpr std::size_t kEvidenceGeneratedWall = 8;
    constexpr std::size_t kBootId = 9;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed.front() == '#')
            continue;
        const auto separator = trimmed.find('=');
        if (separator == std::string::npos || separator == 0) {
            return {.error = "clock evidence line " + std::to_string(line_number) + " must be a key=value record"};
        }
        const auto key = trim_copy(std::string_view(trimmed).substr(0, separator));
        const auto value = trim_copy(std::string_view(trimmed).substr(separator + 1));
        const auto duplicate = [&](std::size_t index) {
            if (seen[index])
                return true;
            seen[index] = true;
            return false;
        };
        if (key == "sync_source") {
            if (duplicate(kSyncSource) || value.empty())
                return {.error = "clock evidence line " + std::to_string(line_number) +
                                 " has a duplicate or empty sync_source"};
            evidence.sync_source = value;
        } else if (key == "sync_status_ok") {
            const auto parsed = parse_bool(value);
            if (duplicate(kSyncStatus) || !parsed.has_value())
                return {.error = "clock evidence line " + std::to_string(line_number) +
                                 " has an invalid or duplicate sync_status_ok"};
            evidence.sync_status_ok = *parsed;
        } else if (key == "wall_offset_ns") {
            const auto parsed = parse_i64(value);
            if (duplicate(kWallOffset) || !parsed.has_value())
                return {.error = "clock evidence line " + std::to_string(line_number) +
                                 " has an invalid or duplicate wall_offset_ns"};
            evidence.wall_offset_ns = *parsed;
        } else if (key == "offset_uncertainty_ns") {
            const auto parsed = parse_u64(value);
            if (duplicate(kOffsetUncertainty) || !parsed.has_value())
                return {.error = "clock evidence line " + std::to_string(line_number) +
                                 " has an invalid or duplicate offset_uncertainty_ns"};
            evidence.offset_uncertainty_ns = *parsed;
        } else if (key == "monotonic_clock_id") {
            if (duplicate(kClockId) || value.empty())
                return {.error = "clock evidence line " + std::to_string(line_number) +
                                 " has a duplicate or empty monotonic_clock_id"};
            evidence.monotonic_clock_id = value;
        } else if (key == "current_local_wall_ns") {
            const auto parsed = parse_i64(value);
            if (duplicate(kCurrentWall) || !parsed.has_value())
                return {.error = "clock evidence line " + std::to_string(line_number) +
                                 " has an invalid or duplicate current_local_wall_ns"};
            evidence.current_local_wall_ns = *parsed;
        } else if (key == "current_local_monotonic_ns") {
            const auto parsed = parse_u64(value);
            if (duplicate(kCurrentMonotonic) || !parsed.has_value())
                return {.error = "clock evidence line " + std::to_string(line_number) +
                                 " has an invalid or duplicate current_local_monotonic_ns"};
            evidence.current_local_monotonic_ns = *parsed;
        } else if (key == "sync_status_monotonic_ns") {
            const auto parsed = parse_u64(value);
            if (duplicate(kSyncMonotonic) || !parsed.has_value())
                return {.error = "clock evidence line " + std::to_string(line_number) +
                                 " has an invalid or duplicate sync_status_monotonic_ns"};
            evidence.sync_status_monotonic_ns = *parsed;
        } else if (key == "evidence_generated_wall_ns") {
            const auto parsed = parse_i64(value);
            if (duplicate(kEvidenceGeneratedWall) || !parsed.has_value())
                return {.error = "clock evidence line " + std::to_string(line_number) +
                                 " has an invalid or duplicate evidence_generated_wall_ns"};
            evidence.evidence_generated_wall_ns = *parsed;
        } else if (key == "boot_id") {
            if (duplicate(kBootId) || value.empty())
                return {.error = "clock evidence line " + std::to_string(line_number) +
                                 " has an invalid or duplicate boot_id"};
            evidence.boot_id = value;
        } else if (key == "sample") {
            std::vector<std::string_view> fields;
            std::size_t begin = 0;
            while (begin <= value.size()) {
                const auto comma = value.find(',', begin);
                const auto end = comma == std::string::npos ? value.size() : comma;
                fields.push_back(std::string_view(value).substr(begin, end - begin));
                if (comma == std::string::npos)
                    break;
                begin = comma + 1;
            }
            if (fields.size() != 6 || fields[5].empty())
                return {.error = "clock evidence line " + std::to_string(line_number) +
                                 " sample requires six comma-separated fields"};
            const auto local_wall = parse_i64(trim_copy(fields[0]));
            const auto local_monotonic = parse_u64(trim_copy(fields[1]));
            const auto exchange_wall = parse_i64(trim_copy(fields[2]));
            const auto transport_delay = parse_u64(trim_copy(fields[3]));
            const auto event_age = parse_u64(trim_copy(fields[4]));
            const auto provenance = trim_copy(fields[5]);
            if (!local_wall.has_value() || !local_monotonic.has_value() || !exchange_wall.has_value() ||
                !transport_delay.has_value() || !event_age.has_value() || provenance.empty()) {
                return {.error = "clock evidence line " + std::to_string(line_number) + " contains an invalid sample"};
            }
            evidence.paired_samples.push_back({.local_wall_ns = *local_wall,
                                               .local_monotonic_ns = *local_monotonic,
                                               .exchange_wall_ns = *exchange_wall,
                                               .provenance = provenance,
                                               .current_reference = true,
                                               .transport_delay_ns = *transport_delay,
                                               .exchange_event_age_ns = *event_age});
        } else {
            return {.error = "clock evidence line " + std::to_string(line_number) + " uses an unknown key: " + key};
        }
    }
    if (!input.eof())
        return {.error = "clock evidence file read failed: " + path.string()};
    return {.evidence = std::move(evidence)};
}

std::string credential_source_name(Plaza2CredentialSource source) {
    switch (source) {
    case Plaza2CredentialSource::None:
        return "none";
    case Plaza2CredentialSource::Env:
        return "env";
    case Plaza2CredentialSource::File:
        return "file";
    }
    return "none";
}

std::optional<RunnerArgs> parse_args(int argc, char** argv) {
    RunnerArgs args;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--profile-id" && index + 1 < argc) {
            args.profile_id = argv[++index];
        } else if (argument == "--output-dir" && index + 1 < argc) {
            args.output_dir = argv[++index];
        } else if (argument == "--endpoint-host" && index + 1 < argc) {
            args.endpoint_host = argv[++index];
        } else if (argument == "--endpoint-port" && index + 1 < argc) {
            const auto parsed = parse_port(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --endpoint-port value\n";
                return std::nullopt;
            }
            args.endpoint_port = *parsed;
        } else if (argument == "--runtime-root" && index + 1 < argc) {
            args.runtime_root = argv[++index];
        } else if (argument == "--library-path" && index + 1 < argc) {
            args.library_path = argv[++index];
        } else if (argument == "--scheme-dir" && index + 1 < argc) {
            args.scheme_dir = argv[++index];
        } else if (argument == "--config-dir" && index + 1 < argc) {
            args.config_dir = argv[++index];
        } else if (argument == "--env-open-settings" && index + 1 < argc) {
            args.env_open_settings = argv[++index];
        } else if (argument == "--expected-spectra-release" && index + 1 < argc) {
            args.expected_spectra_release = argv[++index];
        } else if (argument == "--expected-runtime-library-sha256" && index + 1 < argc) {
            args.expected_runtime_library_sha256 = argv[++index];
        } else if (argument == "--expected-scheme-sha256" && index + 1 < argc) {
            args.expected_scheme_sha256 = argv[++index];
        } else if (argument == "--connection-settings" && index + 1 < argc) {
            args.connection_settings = argv[++index];
        } else if (argument == "--connection-open-settings" && index + 1 < argc) {
            args.connection_open_settings = argv[++index];
        } else if (argument == "--stream-settings" && index + 1 < argc) {
            args.stream_settings = argv[++index];
        } else if (argument == "--stream-open-settings" && index + 1 < argc) {
            args.stream_open_settings = argv[++index];
        } else if (argument == "--process-timeout-ms" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --process-timeout-ms value\n";
                return std::nullopt;
            }
            args.process_timeout_ms = *parsed;
        } else if (argument == "--credentials-source" && index + 1 < argc) {
            const std::string source = argv[++index];
            if (source == "none") {
                args.credentials_source = Plaza2CredentialSource::None;
            } else if (source == "env") {
                args.credentials_source = Plaza2CredentialSource::Env;
            } else if (source == "file") {
                args.credentials_source = Plaza2CredentialSource::File;
            } else {
                std::cerr << "invalid --credentials-source value\n";
                return std::nullopt;
            }
        } else if (argument == "--credentials-env-var" && index + 1 < argc) {
            args.credentials_env_var = argv[++index];
        } else if (argument == "--credentials-file" && index + 1 < argc) {
            args.credentials_file = argv[++index];
        } else if (argument == "--software-key-source" && index + 1 < argc) {
            const std::string source = argv[++index];
            if (source == "none") {
                args.software_key_source = Plaza2CredentialSource::None;
            } else if (source == "env") {
                args.software_key_source = Plaza2CredentialSource::Env;
            } else if (source == "file") {
                args.software_key_source = Plaza2CredentialSource::File;
            } else {
                std::cerr << "invalid --software-key-source value\n";
                return std::nullopt;
            }
        } else if (argument == "--software-key-env-var" && index + 1 < argc) {
            args.software_key_env_var = argv[++index];
        } else if (argument == "--software-key-file" && index + 1 < argc) {
            args.software_key_file = argv[++index];
        } else if (argument == "--armed-test-network") {
            args.armed_test_network = true;
        } else if (argument == "--armed-test-session") {
            args.armed_test_session = true;
        } else if (argument == "--armed-test-plaza2") {
            args.armed_test_plaza2 = true;
        } else if (argument == "--armed-test-market-data") {
            args.armed_test_market_data = true;
        } else if (argument == "--clock-evidence-file" && index + 1 < argc) {
            args.clock_evidence_file = argv[++index];
        } else if (argument == "--max-polls" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --max-polls value\n";
                return std::nullopt;
            }
            args.max_polls = *parsed;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            return std::nullopt;
        }
    }

    if (args.profile_id.empty() || args.output_dir.empty()) {
        std::cerr << "--profile-id and --output-dir are required\n";
        return std::nullopt;
    }
    return args;
}

void write_lines(const fs::path& path, const std::vector<std::string>& lines) {
    std::ofstream out(path);
    for (const auto& line : lines) {
        out << line << '\n';
    }
}

void write_summary_json(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& fields) {
    std::ofstream out(path);
    out << "{\n";
    for (std::size_t index = 0; index < fields.size(); ++index) {
        out << "  \"" << escape_json(fields[index].first) << "\": \"" << escape_json(fields[index].second) << "\"";
        out << (index + 1 == fields.size() ? "\n" : ",\n");
    }
    out << "}\n";
}

std::string level_summary(const std::optional<Plaza2Aggr20Level>& level) {
    if (!level.has_value()) {
        return "not_available";
    }
    return "isin_id=" + std::to_string(level->isin_id) + ";price=" + level->price +
           ";volume=" + std::to_string(level->volume);
}

std::string error_summary(const std::optional<moex::plaza2::cgate::Plaza2Error>& error) {
    if (!error.has_value()) {
        return "none";
    }
    return "code=" + std::to_string(static_cast<int>(error->code)) +
           ";runtime_code=" + std::to_string(error->runtime_code) + ";message=" + error->message;
}

void write_snapshot_json(const fs::path& path, const moex::plaza2::cgate::Plaza2Aggr20Snapshot& snapshot) {
    std::ofstream out(path);
    out << "{\n";
    out << "  \"row_count\": \"" << snapshot.row_count << "\",\n";
    out << "  \"instrument_count\": \"" << snapshot.instrument_count << "\",\n";
    out << "  \"bid_depth_levels\": \"" << snapshot.bid_depth_levels << "\",\n";
    out << "  \"ask_depth_levels\": \"" << snapshot.ask_depth_levels << "\",\n";
    out << "  \"last_repl_id\": \"" << snapshot.last_repl_id << "\",\n";
    out << "  \"last_repl_rev\": \"" << snapshot.last_repl_rev << "\",\n";
    out << "  \"top_bid\": \"" << escape_json(level_summary(snapshot.top_bid)) << "\",\n";
    out << "  \"top_ask\": \"" << escape_json(level_summary(snapshot.top_ask)) << "\"\n";
    out << "}\n";
}

Plaza2Aggr20MdConfig make_config(const RunnerArgs& args) {
    Plaza2Aggr20MdConfig config;
    config.profile_id = args.profile_id;
    config.endpoint_host = args.endpoint_host;
    config.endpoint_port = args.endpoint_port;
    config.runtime.environment = Plaza2Environment::Test;
    config.runtime.runtime_root = args.runtime_root;
    config.runtime.library_path = args.library_path;
    config.runtime.scheme_dir = args.scheme_dir;
    config.runtime.config_dir = args.config_dir;
    config.runtime.env_open_settings = args.env_open_settings;
    config.runtime.expected_spectra_release = args.expected_spectra_release;
    config.runtime.expected_runtime_library_sha256 = args.expected_runtime_library_sha256;
    config.runtime.expected_scheme_sha256 = args.expected_scheme_sha256;
    config.connection_settings = args.connection_settings;
    config.connection_open_settings = args.connection_open_settings;
    config.stream.settings = args.stream_settings;
    config.stream.open_settings = args.stream_open_settings;
    config.process_timeout_ms = args.process_timeout_ms;
    config.credentials.source = args.credentials_source;
    config.credentials.env_var = args.credentials_env_var;
    config.credentials.file_path = args.credentials_file;
    config.software_key.source = args.software_key_source;
    config.software_key.env_var = args.software_key_env_var;
    config.software_key.file_path = args.software_key_file;
    config.arm_state.test_network_armed = args.armed_test_network;
    config.arm_state.test_session_armed = args.armed_test_session;
    config.arm_state.test_plaza2_armed = args.armed_test_plaza2;
    config.test_market_data_armed = args.armed_test_market_data;
    return config;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto parsed = parse_args(argc, argv);
        if (!parsed.has_value()) {
            return 1;
        }

        const auto args = *parsed;
        std::filesystem::create_directories(args.output_dir);
        const auto log_path = args.output_dir / (args.profile_id + ".aggr20.log");
        const auto summary_path = args.output_dir / (args.profile_id + ".aggr20.summary.json");
        const auto snapshot_path = args.output_dir / "aggr20_snapshot.json";

        const auto clock_evidence = read_clock_evidence(args.clock_evidence_file);
        auto config = make_config(args);
        config.clock_evidence = clock_evidence.evidence;
        Plaza2Aggr20MdRunner runner(std::move(config));
        auto result = runner.start();
        if (!clock_evidence.error.empty() && !result.ok)
            result.message += "; " + clock_evidence.error;
        if (result.ok) {
            for (std::uint32_t poll = 0; poll < args.max_polls && !runner.health_snapshot().ready; ++poll) {
                result = runner.poll_once();
                if (!result.ok) {
                    break;
                }
            }
            if (result.ok && !runner.health_snapshot().ready) {
                result = {
                    .ok = false,
                    .message = "AGGR20 TEST runner did not reach ready state before --max-polls elapsed",
                };
            }
        }

        const auto& health = runner.health_snapshot();
        auto lines = runner.operator_log_lines();
        lines.push_back("state=" + std::string(plaza2_aggr20_md_runner_state_name(health.state)));
        lines.push_back("ready=" + std::string(health.ready ? "true" : "false"));
        lines.push_back("runtime_probe_ok=" + std::string(health.runtime_probe_ok ? "true" : "false"));
        lines.push_back("scheme_drift_ok=" + std::string(health.scheme_drift_ok ? "true" : "false"));
        lines.push_back("scheme_drift_status=" + std::string(plaza2_compatibility_name(health.scheme_drift_status)));
        lines.push_back("credentials_source=" + credential_source_name(args.credentials_source));
        lines.push_back("clock_evidence_file=" + args.clock_evidence_file.string());
        lines.push_back("clock_evidence_present=" + std::string(health.clock_evidence_present ? "true" : "false"));
        lines.push_back("clock_evidence_ok=" + std::string(health.clock_evidence_ok ? "true" : "false"));
        if (clock_evidence.evidence.has_value()) {
            lines.push_back("evidence_generated_wall_ns=" +
                            std::to_string(clock_evidence.evidence->evidence_generated_wall_ns.value_or(0)));
            lines.push_back("boot_id_present=" +
                            std::string(clock_evidence.evidence->boot_id.has_value() ? "true" : "false"));
        }
        if (!clock_evidence.error.empty())
            lines.push_back("clock_evidence_error=" + clock_evidence.error);
        lines.push_back("stream_opened=" + std::string(health.stream_opened ? "true" : "false"));
        lines.push_back("stream_online=" + std::string(health.stream_online ? "true" : "false"));
        lines.push_back("stream_snapshot_complete=" + std::string(health.stream_snapshot_complete ? "true" : "false"));
        lines.push_back("transport_active=" + std::string(health.transport_active ? "true" : "false"));
        lines.push_back("session_data_ready=" + std::string(health.session_data_ready ? "true" : "false"));
        lines.push_back("target_authoritative=" + std::string(health.target_authoritative ? "true" : "false"));
        lines.push_back("recovery_service=" + health.recovery_service);
        lines.push_back("reopen_retry_count=" + std::to_string(health.reopen_retry_count));
        lines.push_back("first_recovery_error=" + error_summary(health.first_recovery_error));
        lines.push_back("current_recovery_error=" + error_summary(health.current_recovery_error));
        lines.push_back("row_count=" + std::to_string(health.snapshot.row_count));
        lines.push_back("instrument_count=" + std::to_string(health.snapshot.instrument_count));
        lines.push_back("failure_classification=" + health.failure_classification);
        if (!health.last_error.empty()) {
            lines.push_back("last_error=" + health.last_error);
        }
        write_lines(log_path, lines);
        write_snapshot_json(snapshot_path, health.snapshot);

        write_summary_json(
            summary_path,
            {
                {"profile_id", args.profile_id},
                {"result", result.ok ? "ok" : "failed"},
                {"runner_state", std::string(plaza2_aggr20_md_runner_state_name(health.state))},
                {"ready", health.ready ? "true" : "false"},
                {"runtime_probe_ok", health.runtime_probe_ok ? "true" : "false"},
                {"scheme_drift_ok", health.scheme_drift_ok ? "true" : "false"},
                {"scheme_drift_status", std::string(plaza2_compatibility_name(health.scheme_drift_status))},
                {"scheme_drift_warning_count", std::to_string(health.scheme_drift_warning_count)},
                {"scheme_drift_fatal_count", std::to_string(health.scheme_drift_fatal_count)},
                {"clock_evidence_file", args.clock_evidence_file.string()},
                {"clock_evidence_present", health.clock_evidence_present ? "true" : "false"},
                {"clock_evidence_ok", health.clock_evidence_ok ? "true" : "false"},
                {"clock_evidence_error", clock_evidence.error},
                {"evidence_generated_wall_ns",
                 clock_evidence.evidence.has_value()
                     ? std::to_string(clock_evidence.evidence->evidence_generated_wall_ns.value_or(0))
                     : "0"},
                {"boot_id_present", clock_evidence.evidence.has_value() && clock_evidence.evidence->boot_id.has_value()
                                        ? "true"
                                        : "false"},
                {"stream_opened", health.stream_opened ? "true" : "false"},
                {"stream_online", health.stream_online ? "true" : "false"},
                {"stream_snapshot_complete", health.stream_snapshot_complete ? "true" : "false"},
                {"transport_active", health.transport_active ? "true" : "false"},
                {"session_data_ready", health.session_data_ready ? "true" : "false"},
                {"target_authoritative", health.target_authoritative ? "true" : "false"},
                {"recovery_service", health.recovery_service},
                {"reopen_retry_count", std::to_string(health.reopen_retry_count)},
                {"first_recovery_error", error_summary(health.first_recovery_error)},
                {"current_recovery_error", error_summary(health.current_recovery_error)},
                {"row_count", std::to_string(health.snapshot.row_count)},
                {"instrument_count", std::to_string(health.snapshot.instrument_count)},
                {"bid_depth_levels", std::to_string(health.snapshot.bid_depth_levels)},
                {"ask_depth_levels", std::to_string(health.snapshot.ask_depth_levels)},
                {"top_bid", level_summary(health.snapshot.top_bid)},
                {"top_ask", level_summary(health.snapshot.top_ask)},
                {"last_repl_id", std::to_string(health.snapshot.last_repl_id)},
                {"last_repl_rev", std::to_string(health.snapshot.last_repl_rev)},
                {"failure_classification", health.failure_classification},
                {"last_error", health.last_error},
            });

        static_cast<void>(runner.stop());
        if (!result.ok) {
            std::cerr << result.message << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
