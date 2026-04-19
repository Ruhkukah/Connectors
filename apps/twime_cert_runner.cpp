#include "moex/twime_trade/twime_cert_scenario_runner.hpp"
#include "moex/twime_trade/twime_live_session_runner.hpp"
#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_trade/transport/twime_credential_provider.hpp"
#include "moex/twime_trade/transport/twime_credential_redaction.hpp"
#include "moex/twime_trade/transport/twime_endpoint_resolver.hpp"
#include "moex/twime_trade/transport/twime_test_network_gate.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct RunnerArgs {
    std::string scenario_id;
    fs::path output_dir;

    std::string profile_id;
    std::string endpoint_host;
    std::uint16_t endpoint_port{0};
    bool external_test_endpoint_enabled{false};
    bool require_explicit_runtime_arm{true};
    bool block_production_like_hostnames{true};
    bool block_private_nonlocal_networks_by_default{false};
    bool armed_test_network{false};
    bool armed_test_session{false};
    bool validate_only{false};
    std::string credentials_source{"none"};
    std::string credentials_env_var;
    std::string credentials_file;
    bool reconnect_enabled{false};
    std::uint32_t max_reconnect_attempts{3};
    std::uint32_t establish_deadline_ms{10000};
    std::uint32_t graceful_terminate_timeout_ms{3000};
    std::uint32_t connect_timeout_ms{10000};
    std::uint32_t terminate_timeout_ms{3000};
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

bool parse_bool_flag(std::string_view value) {
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on";
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

std::string state_name(moex::twime_trade::TwimeSessionState state) {
    using moex::twime_trade::TwimeSessionState;
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

bool is_live_session_scenario(std::string_view scenario_id) {
    return scenario_id.rfind("twime_live_test_session_", 0) == 0;
}

std::optional<RunnerArgs> parse_args(int argc, char** argv) {
    RunnerArgs args;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--scenario-id" && index + 1 < argc) {
            args.scenario_id = argv[++index];
            continue;
        }
        if (argument == "--output-dir" && index + 1 < argc) {
            args.output_dir = argv[++index];
            continue;
        }
        if (argument == "--profile-id" && index + 1 < argc) {
            args.profile_id = argv[++index];
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
        if (argument == "--external-test-endpoint-enabled" && index + 1 < argc) {
            args.external_test_endpoint_enabled = parse_bool_flag(argv[++index]);
            continue;
        }
        if (argument == "--require-explicit-runtime-arm" && index + 1 < argc) {
            args.require_explicit_runtime_arm = parse_bool_flag(argv[++index]);
            continue;
        }
        if (argument == "--block-production-like-hostnames" && index + 1 < argc) {
            args.block_production_like_hostnames = parse_bool_flag(argv[++index]);
            continue;
        }
        if (argument == "--block-private-nonlocal-networks" && index + 1 < argc) {
            args.block_private_nonlocal_networks_by_default = parse_bool_flag(argv[++index]);
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
        if (argument == "--validate-only") {
            args.validate_only = true;
            continue;
        }
        if (argument == "--credentials-source" && index + 1 < argc) {
            args.credentials_source = argv[++index];
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
        if (argument == "--reconnect-enabled" && index + 1 < argc) {
            args.reconnect_enabled = parse_bool_flag(argv[++index]);
            continue;
        }
        if (argument == "--max-reconnect-attempts" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --max-reconnect-attempts value\n";
                return std::nullopt;
            }
            args.max_reconnect_attempts = *parsed;
            continue;
        }
        if (argument == "--establish-deadline-ms" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --establish-deadline-ms value\n";
                return std::nullopt;
            }
            args.establish_deadline_ms = *parsed;
            continue;
        }
        if (argument == "--graceful-terminate-timeout-ms" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --graceful-terminate-timeout-ms value\n";
                return std::nullopt;
            }
            args.graceful_terminate_timeout_ms = *parsed;
            continue;
        }
        if (argument == "--connect-timeout-ms" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --connect-timeout-ms value\n";
                return std::nullopt;
            }
            args.connect_timeout_ms = *parsed;
            continue;
        }
        if (argument == "--terminate-timeout-ms" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --terminate-timeout-ms value\n";
                return std::nullopt;
            }
            args.terminate_timeout_ms = *parsed;
            continue;
        }
        std::cerr << "unknown argument: " << argument << '\n';
        return std::nullopt;
    }

    if (args.output_dir.empty()) {
        std::cerr << "missing --output-dir\n";
        return std::nullopt;
    }

    const bool scenario_mode = !args.scenario_id.empty();
    const bool profile_mode = !args.profile_id.empty();
    if (!scenario_mode && !profile_mode) {
        std::cerr << "provide --scenario-id and/or --profile-id\n";
        return std::nullopt;
    }

    if (profile_mode && (args.endpoint_host.empty() || args.endpoint_port == 0)) {
        std::cerr << "profile mode requires --endpoint-host and --endpoint-port\n";
        return std::nullopt;
    }

    return args;
}

moex::twime_trade::transport::TwimeCredentialConfig make_credential_config(const RunnerArgs& args) {
    using moex::twime_trade::transport::TwimeCredentialConfig;
    using moex::twime_trade::transport::TwimeCredentialSource;

    TwimeCredentialConfig config;
    if (args.credentials_source == "env") {
        config.source = TwimeCredentialSource::Env;
        config.env_var = args.credentials_env_var;
    } else if (args.credentials_source == "file") {
        config.source = TwimeCredentialSource::File;
        config.file_path = args.credentials_file;
    } else {
        config.source = TwimeCredentialSource::None;
    }
    return config;
}

int run_scenario_mode(const RunnerArgs& args) {
    const auto scenario = moex::twime_trade::TwimeCertScenarioRunner::builtin(args.scenario_id);
    if (!scenario.has_value()) {
        std::cerr << "unknown TWIME synthetic scenario: " << args.scenario_id << '\n';
        return 2;
    }

    const auto result = moex::twime_trade::TwimeCertScenarioRunner{}.run(*scenario);
    if (!result.error_message.empty()) {
        std::cerr << "scenario failed: " << result.error_message << '\n';
        return 1;
    }

    fs::create_directories(args.output_dir);
    const auto log_path = args.output_dir / (args.scenario_id + ".cert.log");
    write_lines(log_path, result.cert_log_lines);

    write_summary_json(args.output_dir / (args.scenario_id + ".summary.json"),
                       {
                           {"scenario_id", result.scenario_id},
                           {"title", result.title},
                           {"final_state", state_name(result.final_state)},
                           {"event_count", std::to_string(result.events.size())},
                           {"log_path", log_path.string()},
                       });

    std::cout << "synthetic TWIME cert log emitted: " << log_path << '\n';
    return 0;
}

int run_profile_mode(const RunnerArgs& args) {
    namespace transport = moex::twime_trade::transport;

    transport::TwimeTcpConfig tcp_config;
    tcp_config.environment = transport::TwimeTcpEnvironment::Test;
    tcp_config.endpoint.host = args.endpoint_host;
    tcp_config.endpoint.port = args.endpoint_port;
    tcp_config.test_network_gate.external_test_endpoint_enabled = args.external_test_endpoint_enabled;
    tcp_config.test_network_gate.require_explicit_runtime_arm = args.require_explicit_runtime_arm;
    tcp_config.test_network_gate.block_production_like_hostnames = args.block_production_like_hostnames;
    tcp_config.test_network_gate.block_private_nonlocal_networks_by_default =
        args.block_private_nonlocal_networks_by_default;
    tcp_config.runtime_arm_state.test_network_armed = args.armed_test_network;
    tcp_config.runtime_arm_state.test_session_armed = args.armed_test_session;

    const auto validation =
        transport::TwimeTestNetworkGate(tcp_config.runtime_arm_state, tcp_config).validate_before_open();
    const auto credential_config = make_credential_config(args);
    const bool require_credentials =
        args.external_test_endpoint_enabled || !transport::twime_is_explicit_loopback_host(args.endpoint_host);
    const auto credentials = transport::load_twime_credentials(credential_config);

    fs::create_directories(args.output_dir);
    const auto log_path = args.output_dir / (args.profile_id + ".cert.log");
    const auto summary_path = args.output_dir / (args.profile_id + ".summary.json");

    std::vector<std::string> log_lines;
    log_lines.push_back(transport::format_twime_test_network_banner(args.armed_test_network) +
                        " profile_id=" + args.profile_id);
    if (args.armed_test_session) {
        log_lines.push_back("[TEST-SESSION-ARMED]");
    }
    log_lines.push_back("endpoint_host=" + args.endpoint_host + " port=" + std::to_string(args.endpoint_port));
    log_lines.push_back("endpoint_validation=" + validation.summary);

    if (!validation.allowed) {
        write_lines(log_path, log_lines);
        write_summary_json(summary_path,
                           {
                               {"profile_id", args.profile_id},
                               {"mode", "test_endpoint_validation"},
                               {"status", "blocked"},
                               {"reason", validation.summary},
                               {"error_code", std::to_string(static_cast<unsigned>(validation.error_code))},
                               {"log_path", log_path.string()},
                           });
        std::cerr << validation.summary << '\n';
        return 1;
    }

    if (require_credentials && !credentials.has_value()) {
        log_lines.push_back("credentials=" + transport::redact_twime_credentials("") + " [REDACTED_CREDENTIALS]");
        write_lines(log_path, log_lines);
        write_summary_json(
            summary_path, {
                              {"profile_id", args.profile_id},
                              {"mode", "test_endpoint_validation"},
                              {"status", "missing_credentials"},
                              {"reason", "external test-network mode requires credentials from local env/file sources"},
                              {"log_path", log_path.string()},
                          });
        std::cerr << "external test-network mode requires credentials from local env/file sources\n";
        return 1;
    }

    if (credentials.has_value()) {
        log_lines.push_back("credentials=" + transport::redact_twime_credentials(credentials->credentials) +
                            " [REDACTED_CREDENTIALS]");
    }

    const std::string scenario_id = args.scenario_id.empty() ? "twime_live_test_session_establish" : args.scenario_id;
    const bool live_session = is_live_session_scenario(scenario_id);
    if (!args.validate_only && live_session && !transport::twime_is_explicit_loopback_host(args.endpoint_host) &&
        !args.armed_test_session) {
        write_lines(log_path, log_lines);
        write_summary_json(summary_path, {
                                             {"profile_id", args.profile_id},
                                             {"mode", "live_test_session"},
                                             {"status", "blocked"},
                                             {"reason", "external TWIME test session requires --armed-test-session"},
                                             {"log_path", log_path.string()},
                                         });
        std::cerr << "external TWIME test session requires --armed-test-session\n";
        return 1;
    }

    if (args.validate_only) {
        log_lines.push_back("validate_only=true");
        write_lines(log_path, log_lines);
        write_summary_json(summary_path, {
                                             {"profile_id", args.profile_id},
                                             {"mode", "test_endpoint_validation"},
                                             {"status", "validated"},
                                             {"log_path", log_path.string()},
                                         });
        std::cout << "TWIME test endpoint validation passed: " << log_path << '\n';
        return 0;
    }

    moex::twime_trade::TwimeLiveSessionConfig live_config;
    live_config.session.session_id = args.profile_id;
    live_config.session.credentials = credentials.has_value() ? credentials->credentials : "LOGIN";
    live_config.tcp = tcp_config;
    live_config.credentials = credential_config;
    live_config.policy.reconnect_enabled = args.reconnect_enabled;
    live_config.policy.max_reconnect_attempts = args.max_reconnect_attempts;
    live_config.policy.establish_deadline_ms = args.establish_deadline_ms;
    live_config.policy.graceful_terminate_timeout_ms = args.graceful_terminate_timeout_ms;

    moex::twime_trade::TwimeFakeClock clock(0);
    moex::twime_trade::TwimeFileSessionPersistenceStore persistence_store(args.output_dir / "state");
    moex::twime_trade::TwimeLiveSessionRunner runner(live_config, persistence_store, clock);

    const auto start_result = runner.start();
    if (!start_result.ok) {
        log_lines.insert(log_lines.end(), runner.operator_log_lines().begin(), runner.operator_log_lines().end());
        write_lines(log_path, log_lines);
        write_summary_json(summary_path, {
                                             {"profile_id", args.profile_id},
                                             {"mode", live_session ? "live_test_session" : "test_endpoint_connect"},
                                             {"status", "blocked"},
                                             {"reason", start_result.message},
                                             {"log_path", log_path.string()},
                                         });
        std::cerr << start_result.message << '\n';
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(args.connect_timeout_ms);
    bool success = false;
    std::string failure_reason;

    while (std::chrono::steady_clock::now() < deadline) {
        const auto poll_result = runner.poll_once();
        if (!poll_result.ok) {
            failure_reason = poll_result.message;
            break;
        }

        const auto state = runner.health_snapshot().state;
        if (scenario_id == "twime_live_test_session_reject") {
            if (state == moex::twime_trade::TwimeSessionState::Rejected) {
                success = true;
                break;
            }
        } else if (scenario_id == "twime_live_test_session_reconnect_disabled") {
            if (state == moex::twime_trade::TwimeSessionState::Faulted) {
                success = true;
                break;
            }
        } else if (scenario_id == "twime_live_test_session_reconnect_enabled") {
            if (runner.session_metrics().reconnect_attempts >= 2) {
                success = true;
                break;
            }
        } else if (scenario_id == "twime_live_test_session_heartbeat") {
            if (state == moex::twime_trade::TwimeSessionState::Active &&
                (runner.health_snapshot().heartbeat_sent > 0 || runner.health_snapshot().heartbeat_received > 0)) {
                success = true;
                break;
            }
        } else {
            if (state == moex::twime_trade::TwimeSessionState::Active) {
                success = true;
                break;
            }
        }

        if (state == moex::twime_trade::TwimeSessionState::Rejected ||
            state == moex::twime_trade::TwimeSessionState::Faulted) {
            failure_reason = "live test session entered terminal failure state";
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const bool stop_after_active = scenario_id == "twime_live_test_session_establish" ||
                                   scenario_id == "twime_live_test_session_heartbeat" ||
                                   scenario_id == "twime_live_test_session_terminate";
    if (success && stop_after_active) {
        (void)runner.request_stop();
        const auto terminate_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(args.terminate_timeout_ms);
        while (std::chrono::steady_clock::now() < terminate_deadline) {
            const auto poll_result = runner.poll_once();
            if (!poll_result.ok) {
                failure_reason = poll_result.message;
                success = false;
                break;
            }
            (void)runner.stop_if_needed();
            if (runner.health_snapshot().state == moex::twime_trade::TwimeSessionState::Terminated) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    log_lines.insert(log_lines.end(), runner.operator_log_lines().begin(), runner.operator_log_lines().end());
    log_lines.insert(log_lines.end(), runner.cert_log_lines().begin(), runner.cert_log_lines().end());
    write_lines(log_path, log_lines);
    write_summary_json(summary_path, {
                                         {"profile_id", args.profile_id},
                                         {"mode", live_session ? "live_test_session" : "test_endpoint_connect"},
                                         {"scenario_id", scenario_id},
                                         {"final_state", state_name(runner.health_snapshot().state)},
                                         {"bytes_read", std::to_string(runner.health_snapshot().bytes_read)},
                                         {"bytes_written", std::to_string(runner.health_snapshot().bytes_written)},
                                         {"log_path", log_path.string()},
                                     });

    if (!success) {
        if (failure_reason.empty()) {
            failure_reason = "live test session did not reach the expected scenario outcome";
        }
        std::cerr << failure_reason << '\n';
        return 1;
    }

    std::cout << "TWIME live test session log emitted: " << log_path << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto args = parse_args(argc, argv);
    if (!args.has_value()) {
        return 2;
    }

    if (!args->profile_id.empty()) {
        return run_profile_mode(*args);
    }
    if (!args->scenario_id.empty()) {
        return run_scenario_mode(*args);
    }
    return 2;
}
