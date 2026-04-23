#include "moex/plaza2/cgate/plaza2_live_session_runner.hpp"

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

using moex::plaza2::cgate::Plaza2CredentialSource;
using moex::plaza2::cgate::Plaza2Environment;
using moex::plaza2::cgate::Plaza2LiveRunnerState;
using moex::plaza2::cgate::Plaza2LiveSessionConfig;
using moex::plaza2::cgate::Plaza2LiveSessionRunner;
using moex::plaza2::cgate::Plaza2LiveStreamConfig;

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
    std::string expected_scheme_sha256;
    std::string connection_settings;
    std::string connection_open_settings;
    std::vector<std::pair<std::string, std::string>> stream_settings;
    std::vector<std::pair<std::string, std::string>> stream_open_settings;
    std::uint32_t process_timeout_ms{50};
    Plaza2CredentialSource credentials_source{Plaza2CredentialSource::None};
    std::string credentials_env_var;
    fs::path credentials_file;
    bool armed_test_network{false};
    bool armed_test_session{false};
    bool armed_test_plaza2{false};
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

std::optional<std::pair<std::string, std::string>> parse_assignment(std::string_view value) {
    const auto equals = value.find('=');
    if (equals == std::string_view::npos) {
        return std::nullopt;
    }
    return std::pair<std::string, std::string>{
        std::string(value.substr(0, equals)),
        std::string(value.substr(equals + 1)),
    };
}

std::string runner_state_name(Plaza2LiveRunnerState state) {
    switch (state) {
    case Plaza2LiveRunnerState::Created:
        return "Created";
    case Plaza2LiveRunnerState::Validated:
        return "Validated";
    case Plaza2LiveRunnerState::Started:
        return "Started";
    case Plaza2LiveRunnerState::Ready:
        return "Ready";
    case Plaza2LiveRunnerState::Stopped:
        return "Stopped";
    case Plaza2LiveRunnerState::Failed:
        return "Failed";
    }
    return "Unknown";
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
            continue;
        }
        if (argument == "--output-dir" && index + 1 < argc) {
            args.output_dir = argv[++index];
            continue;
        }
        if (argument == "--endpoint-host" && index + 1 < argc) {
            args.endpoint_host = argv[++index];
            continue;
        }
        if (argument == "--endpoint-port" && index + 1 < argc) {
            const auto parsed = parse_port(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --endpoint-port value\n";
                return std::nullopt;
            }
            args.endpoint_port = *parsed;
            continue;
        }
        if (argument == "--runtime-root" && index + 1 < argc) {
            args.runtime_root = argv[++index];
            continue;
        }
        if (argument == "--library-path" && index + 1 < argc) {
            args.library_path = argv[++index];
            continue;
        }
        if (argument == "--scheme-dir" && index + 1 < argc) {
            args.scheme_dir = argv[++index];
            continue;
        }
        if (argument == "--config-dir" && index + 1 < argc) {
            args.config_dir = argv[++index];
            continue;
        }
        if (argument == "--env-open-settings" && index + 1 < argc) {
            args.env_open_settings = argv[++index];
            continue;
        }
        if (argument == "--expected-spectra-release" && index + 1 < argc) {
            args.expected_spectra_release = argv[++index];
            continue;
        }
        if (argument == "--expected-scheme-sha256" && index + 1 < argc) {
            args.expected_scheme_sha256 = argv[++index];
            continue;
        }
        if (argument == "--connection-settings" && index + 1 < argc) {
            args.connection_settings = argv[++index];
            continue;
        }
        if (argument == "--connection-open-settings" && index + 1 < argc) {
            args.connection_open_settings = argv[++index];
            continue;
        }
        if (argument == "--stream-settings" && index + 1 < argc) {
            const auto parsed = parse_assignment(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --stream-settings assignment\n";
                return std::nullopt;
            }
            args.stream_settings.push_back(*parsed);
            continue;
        }
        if (argument == "--stream-open-settings" && index + 1 < argc) {
            const auto parsed = parse_assignment(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --stream-open-settings assignment\n";
                return std::nullopt;
            }
            args.stream_open_settings.push_back(*parsed);
            continue;
        }
        if (argument == "--process-timeout-ms" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --process-timeout-ms value\n";
                return std::nullopt;
            }
            args.process_timeout_ms = *parsed;
            continue;
        }
        if (argument == "--credentials-source" && index + 1 < argc) {
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
            continue;
        }
        if (argument == "--credentials-env-var" && index + 1 < argc) {
            args.credentials_env_var = argv[++index];
            continue;
        }
        if (argument == "--credentials-file" && index + 1 < argc) {
            args.credentials_file = argv[++index];
            continue;
        }
        if (argument == "--armed-test-network") {
            args.armed_test_network = true;
            continue;
        }
        if (argument == "--armed-test-session") {
            args.armed_test_session = true;
            continue;
        }
        if (argument == "--armed-test-plaza2") {
            args.armed_test_plaza2 = true;
            continue;
        }
        if (argument == "--max-polls" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --max-polls value\n";
                return std::nullopt;
            }
            args.max_polls = *parsed;
            continue;
        }
        std::cerr << "unknown argument: " << argument << '\n';
        return std::nullopt;
    }

    if (args.profile_id.empty() || args.output_dir.empty()) {
        std::cerr << "--profile-id and --output-dir are required\n";
        return std::nullopt;
    }

    return args;
}

std::optional<moex::plaza2::generated::StreamCode> stream_code_from_name(std::string_view stream_name) {
    for (const auto& descriptor : moex::plaza2::generated::StreamDescriptors()) {
        if (descriptor.stream_name == stream_name) {
            return descriptor.stream_code;
        }
    }
    return std::nullopt;
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

Plaza2LiveSessionConfig make_config(const RunnerArgs& args) {
    Plaza2LiveSessionConfig config;
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
    config.runtime.expected_scheme_sha256 = args.expected_scheme_sha256;
    config.connection_settings = args.connection_settings;
    config.connection_open_settings = args.connection_open_settings;
    config.process_timeout_ms = args.process_timeout_ms;
    config.credentials.source = args.credentials_source;
    config.credentials.env_var = args.credentials_env_var;
    config.credentials.file_path = args.credentials_file;
    config.arm_state.test_network_armed = args.armed_test_network;
    config.arm_state.test_session_armed = args.armed_test_session;
    config.arm_state.test_plaza2_armed = args.armed_test_plaza2;

    for (const auto& [stream_name, settings] : args.stream_settings) {
        const auto stream_code = stream_code_from_name(stream_name);
        if (!stream_code.has_value()) {
            throw std::runtime_error("unknown PLAZA II stream name: " + stream_name);
        }
        config.streams.push_back({
            .stream_code = *stream_code,
            .settings = settings,
        });
    }
    for (const auto& [stream_name, open_settings] : args.stream_open_settings) {
        const auto stream_code = stream_code_from_name(stream_name);
        if (!stream_code.has_value()) {
            throw std::runtime_error("unknown PLAZA II stream name: " + stream_name);
        }
        for (auto& stream : config.streams) {
            if (stream.stream_code == *stream_code) {
                stream.open_settings = open_settings;
            }
        }
    }
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
        const auto log_path = args.output_dir / (args.profile_id + ".cert.log");
        const auto summary_path = args.output_dir / (args.profile_id + ".summary.json");

        Plaza2LiveSessionRunner runner(make_config(args));
        auto result = runner.start();
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
                    .message = "PLAZA II TEST runner did not reach ready state before --max-polls elapsed",
                };
            }
        }

        const auto& health = runner.health_snapshot();
        auto lines = runner.operator_log_lines();
        lines.push_back("state=" + runner_state_name(health.state));
        lines.push_back("ready=" + std::string(health.ready ? "true" : "false"));
        lines.push_back("runtime_probe_ok=" + std::string(health.runtime_probe_ok ? "true" : "false"));
        lines.push_back("scheme_drift_ok=" + std::string(health.scheme_drift_ok ? "true" : "false"));
        lines.push_back("credentials_source=" + credential_source_name(args.credentials_source));
        if (!health.last_error.empty()) {
            lines.push_back("last_error=" + health.last_error);
        }
        write_lines(log_path, lines);

        write_summary_json(summary_path,
                           {
                               {"profile_id", args.profile_id},
                               {"result", result.ok ? "ok" : "failed"},
                               {"runner_state", runner_state_name(health.state)},
                               {"ready", health.ready ? "true" : "false"},
                               {"runtime_probe_ok", health.runtime_probe_ok ? "true" : "false"},
                               {"scheme_drift_ok", health.scheme_drift_ok ? "true" : "false"},
                               {"session_count", std::to_string(health.counts.session_count)},
                               {"instrument_count", std::to_string(health.counts.instrument_count)},
                               {"matching_map_count", std::to_string(health.counts.matching_map_count)},
                               {"limit_count", std::to_string(health.counts.limit_count)},
                               {"position_count", std::to_string(health.counts.position_count)},
                               {"own_order_count", std::to_string(health.counts.own_order_count)},
                               {"own_trade_count", std::to_string(health.counts.own_trade_count)},
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
