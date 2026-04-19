#include "moex/twime_trade/twime_cert_scenario_runner.hpp"
#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_trade/transport/twime_credential_provider.hpp"
#include "moex/twime_trade/transport/twime_credential_redaction.hpp"
#include "moex/twime_trade/transport/twime_endpoint_resolver.hpp"
#include "moex/twime_trade/transport/twime_tcp_transport.hpp"
#include "moex/twime_trade/transport/twime_test_network_gate.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

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
    bool validate_only{false};
    std::string credentials_source{"none"};
    std::string credentials_env_var;
    std::string credentials_file;
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
        std::cerr << "unknown argument: " << argument << '\n';
        return std::nullopt;
    }

    if (args.output_dir.empty()) {
        std::cerr << "missing --output-dir\n";
        return std::nullopt;
    }

    const bool scenario_mode = !args.scenario_id.empty();
    const bool profile_mode = !args.profile_id.empty();
    if (scenario_mode == profile_mode) {
        std::cerr << "provide either --scenario-id or --profile-id\n";
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

    const auto validation =
        transport::TwimeTestNetworkGate(tcp_config.runtime_arm_state, tcp_config).validate_before_open();

    fs::create_directories(args.output_dir);
    const auto log_path = args.output_dir / (args.profile_id + ".cert.log");
    const auto summary_path = args.output_dir / (args.profile_id + ".summary.json");

    std::vector<std::string> log_lines;
    log_lines.push_back(transport::format_twime_test_network_banner(args.armed_test_network) +
                        " profile_id=" + args.profile_id);
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

    const auto credential_config = make_credential_config(args);
    const bool require_credentials =
        args.external_test_endpoint_enabled || !transport::twime_is_explicit_loopback_host(args.endpoint_host);
    const auto credentials = transport::load_twime_credentials(credential_config);

    if (require_credentials && !credentials.has_value()) {
        log_lines.push_back("credentials=" + transport::redact_twime_credentials(""));
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

    transport::TwimeTcpTransport tcp_transport(tcp_config);
    moex::twime_trade::TwimeSessionConfig session_config;
    session_config.session_id = args.profile_id;
    session_config.credentials = credentials.has_value() ? credentials->credentials : "LOGIN";
    moex::twime_trade::TwimeInMemoryRecoveryStateStore recovery_store;
    moex::twime_trade::TwimeFakeClock clock(1'715'000'000);
    moex::twime_trade::TwimeSession session(session_config, tcp_transport, recovery_store, clock);

    session.apply_command({moex::twime_trade::TwimeSessionCommandType::ConnectFake});
    for (int attempt = 0; attempt < 128; ++attempt) {
        session.poll_transport();
        const auto state = session.state();
        if (state == moex::twime_trade::TwimeSessionState::Active ||
            state == moex::twime_trade::TwimeSessionState::Rejected ||
            state == moex::twime_trade::TwimeSessionState::Faulted ||
            state == moex::twime_trade::TwimeSessionState::Terminated) {
            break;
        }
    }

    const auto session_logs = session.cert_log_lines();
    log_lines.insert(log_lines.end(), session_logs.begin(), session_logs.end());
    write_lines(log_path, log_lines);
    write_summary_json(summary_path, {
                                         {"profile_id", args.profile_id},
                                         {"mode", "test_endpoint_connect"},
                                         {"final_state", state_name(session.state())},
                                         {"log_path", log_path.string()},
                                     });

    if (session.state() == moex::twime_trade::TwimeSessionState::Faulted ||
        session.state() == moex::twime_trade::TwimeSessionState::Rejected) {
        std::cerr << "TWIME test endpoint session did not become active\n";
        return 1;
    }

    std::cout << "TWIME test endpoint session log emitted: " << log_path << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto args = parse_args(argc, argv);
    if (!args.has_value()) {
        return 2;
    }

    if (!args->scenario_id.empty()) {
        return run_scenario_mode(*args);
    }
    return run_profile_mode(*args);
}
