#include "adapters/alorengine_capi/moex_c_api.h"
#include "moex/plaza2_twime_reconciler/plaza2_twime_integrated_test_runner.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using moex::plaza2::cgate::Plaza2CredentialSource;
using moex::plaza2::cgate::Plaza2Environment;
using moex::plaza2::cgate::Plaza2LiveStreamConfig;
using moex::plaza2::generated::StreamCode;
using moex::plaza2_twime_reconciler::Plaza2TwimeIntegratedRunnerState;
using moex::plaza2_twime_reconciler::Plaza2TwimeIntegratedTestConfig;
using moex::twime_trade::transport::TwimeCredentialSource;
using moex::twime_trade::transport::TwimeTcpEnvironment;

std::atomic<bool> g_stop_requested{false};

struct RunnerArgs {
    std::string profile_id;
    fs::path output_dir;

    std::string twime_endpoint_host;
    std::uint16_t twime_endpoint_port{0};
    bool twime_allow_non_loopback{false};
    bool twime_allow_non_localhost_dns{false};
    bool twime_external_test_endpoint_enabled{false};
    bool twime_require_explicit_runtime_arm{true};
    bool twime_block_production_like_hostnames{true};
    TwimeCredentialSource twime_credentials_source{TwimeCredentialSource::None};
    std::string twime_credentials_env_var;
    fs::path twime_credentials_file;
    bool twime_reconnect_enabled{false};
    std::uint32_t twime_max_reconnect_attempts{3};
    std::uint32_t twime_establish_deadline_ms{10000};
    std::uint32_t twime_graceful_terminate_timeout_ms{3000};

    std::string plaza_endpoint_host;
    std::uint16_t plaza_endpoint_port{0};
    fs::path plaza_runtime_root;
    fs::path plaza_library_path;
    fs::path plaza_scheme_dir;
    fs::path plaza_config_dir;
    std::string plaza_env_open_settings;
    std::string plaza_expected_spectra_release;
    std::string plaza_expected_scheme_sha256;
    std::string plaza_connection_settings;
    std::string plaza_connection_open_settings;
    std::vector<std::pair<std::string, std::string>> plaza_stream_settings;
    std::vector<std::pair<std::string, std::string>> plaza_stream_open_settings;
    std::uint32_t plaza_process_timeout_ms{50};
    Plaza2CredentialSource plaza_credentials_source{Plaza2CredentialSource::None};
    std::string plaza_credentials_env_var;
    fs::path plaza_credentials_file;

    bool armed_test_network{false};
    bool armed_test_session{false};
    bool armed_test_plaza2{false};
    bool armed_test_reconcile{false};

    std::uint64_t reconciler_stale_after_steps{4};
    std::uint32_t max_polls{16};
};

void handle_signal(int) {
    g_stop_requested.store(true);
}

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

std::string bool_string(bool value) {
    return value ? "true" : "false";
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

std::optional<std::uint64_t> parse_u64(std::string_view value) {
    try {
        return static_cast<std::uint64_t>(std::stoull(std::string(value)));
    } catch (...) {
        return std::nullopt;
    }
}

bool parse_bool_flag(std::string_view value) {
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on";
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

std::string runner_state_name(Plaza2TwimeIntegratedRunnerState state) {
    switch (state) {
    case Plaza2TwimeIntegratedRunnerState::Created:
        return "Created";
    case Plaza2TwimeIntegratedRunnerState::Validated:
        return "Validated";
    case Plaza2TwimeIntegratedRunnerState::Started:
        return "Started";
    case Plaza2TwimeIntegratedRunnerState::Ready:
        return "Ready";
    case Plaza2TwimeIntegratedRunnerState::Stopped:
        return "Stopped";
    case Plaza2TwimeIntegratedRunnerState::Failed:
        return "Failed";
    }
    return "Unknown";
}

std::string twime_credential_source_name(TwimeCredentialSource source) {
    switch (source) {
    case TwimeCredentialSource::None:
        return "none";
    case TwimeCredentialSource::Env:
        return "env";
    case TwimeCredentialSource::File:
        return "file";
    }
    return "none";
}

std::string plaza_credential_source_name(Plaza2CredentialSource source) {
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

struct AbiSnapshotCounts {
    MoexResult private_order_result{MOEX_RESULT_NULL_POINTER};
    MoexResult private_trade_result{MOEX_RESULT_NULL_POINTER};
    MoexResult reconciled_order_result{MOEX_RESULT_NULL_POINTER};
    MoexResult reconciled_trade_result{MOEX_RESULT_NULL_POINTER};
    uint32_t private_order_count{0};
    uint32_t private_trade_count{0};
    uint32_t reconciled_order_count{0};
    uint32_t reconciled_trade_count{0};
};

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
        if (argument == "--twime-endpoint-host" && index + 1 < argc) {
            args.twime_endpoint_host = argv[++index];
            continue;
        }
        if (argument == "--twime-endpoint-port" && index + 1 < argc) {
            const auto parsed = parse_port(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --twime-endpoint-port value\n";
                return std::nullopt;
            }
            args.twime_endpoint_port = *parsed;
            continue;
        }
        if (argument == "--twime-allow-non-loopback" && index + 1 < argc) {
            args.twime_allow_non_loopback = parse_bool_flag(argv[++index]);
            continue;
        }
        if (argument == "--twime-allow-non-localhost-dns" && index + 1 < argc) {
            args.twime_allow_non_localhost_dns = parse_bool_flag(argv[++index]);
            continue;
        }
        if (argument == "--twime-external-test-endpoint-enabled" && index + 1 < argc) {
            args.twime_external_test_endpoint_enabled = parse_bool_flag(argv[++index]);
            continue;
        }
        if (argument == "--twime-require-explicit-runtime-arm" && index + 1 < argc) {
            args.twime_require_explicit_runtime_arm = parse_bool_flag(argv[++index]);
            continue;
        }
        if (argument == "--twime-block-production-like-hostnames" && index + 1 < argc) {
            args.twime_block_production_like_hostnames = parse_bool_flag(argv[++index]);
            continue;
        }
        if (argument == "--twime-credentials-source" && index + 1 < argc) {
            const std::string source = argv[++index];
            if (source == "none") {
                args.twime_credentials_source = TwimeCredentialSource::None;
            } else if (source == "env") {
                args.twime_credentials_source = TwimeCredentialSource::Env;
            } else if (source == "file") {
                args.twime_credentials_source = TwimeCredentialSource::File;
            } else {
                std::cerr << "invalid --twime-credentials-source value\n";
                return std::nullopt;
            }
            continue;
        }
        if (argument == "--twime-credentials-env-var" && index + 1 < argc) {
            args.twime_credentials_env_var = argv[++index];
            continue;
        }
        if (argument == "--twime-credentials-file" && index + 1 < argc) {
            args.twime_credentials_file = argv[++index];
            continue;
        }
        if (argument == "--twime-reconnect-enabled" && index + 1 < argc) {
            args.twime_reconnect_enabled = parse_bool_flag(argv[++index]);
            continue;
        }
        if (argument == "--twime-max-reconnect-attempts" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --twime-max-reconnect-attempts value\n";
                return std::nullopt;
            }
            args.twime_max_reconnect_attempts = *parsed;
            continue;
        }
        if (argument == "--twime-establish-deadline-ms" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --twime-establish-deadline-ms value\n";
                return std::nullopt;
            }
            args.twime_establish_deadline_ms = *parsed;
            continue;
        }
        if (argument == "--twime-graceful-terminate-timeout-ms" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --twime-graceful-terminate-timeout-ms value\n";
                return std::nullopt;
            }
            args.twime_graceful_terminate_timeout_ms = *parsed;
            continue;
        }
        if (argument == "--plaza-endpoint-host" && index + 1 < argc) {
            args.plaza_endpoint_host = argv[++index];
            continue;
        }
        if (argument == "--plaza-endpoint-port" && index + 1 < argc) {
            const auto parsed = parse_port(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --plaza-endpoint-port value\n";
                return std::nullopt;
            }
            args.plaza_endpoint_port = *parsed;
            continue;
        }
        if (argument == "--plaza-runtime-root" && index + 1 < argc) {
            args.plaza_runtime_root = argv[++index];
            continue;
        }
        if (argument == "--plaza-library-path" && index + 1 < argc) {
            args.plaza_library_path = argv[++index];
            continue;
        }
        if (argument == "--plaza-scheme-dir" && index + 1 < argc) {
            args.plaza_scheme_dir = argv[++index];
            continue;
        }
        if (argument == "--plaza-config-dir" && index + 1 < argc) {
            args.plaza_config_dir = argv[++index];
            continue;
        }
        if (argument == "--plaza-env-open-settings" && index + 1 < argc) {
            args.plaza_env_open_settings = argv[++index];
            continue;
        }
        if (argument == "--plaza-expected-spectra-release" && index + 1 < argc) {
            args.plaza_expected_spectra_release = argv[++index];
            continue;
        }
        if (argument == "--plaza-expected-scheme-sha256" && index + 1 < argc) {
            args.plaza_expected_scheme_sha256 = argv[++index];
            continue;
        }
        if (argument == "--plaza-connection-settings" && index + 1 < argc) {
            args.plaza_connection_settings = argv[++index];
            continue;
        }
        if (argument == "--plaza-connection-open-settings" && index + 1 < argc) {
            args.plaza_connection_open_settings = argv[++index];
            continue;
        }
        if (argument == "--plaza-stream-settings" && index + 1 < argc) {
            const auto parsed = parse_assignment(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --plaza-stream-settings assignment\n";
                return std::nullopt;
            }
            args.plaza_stream_settings.push_back(*parsed);
            continue;
        }
        if (argument == "--plaza-stream-open-settings" && index + 1 < argc) {
            const auto parsed = parse_assignment(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --plaza-stream-open-settings assignment\n";
                return std::nullopt;
            }
            args.plaza_stream_open_settings.push_back(*parsed);
            continue;
        }
        if (argument == "--plaza-process-timeout-ms" && index + 1 < argc) {
            const auto parsed = parse_u32(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --plaza-process-timeout-ms value\n";
                return std::nullopt;
            }
            args.plaza_process_timeout_ms = *parsed;
            continue;
        }
        if (argument == "--plaza-credentials-source" && index + 1 < argc) {
            const std::string source = argv[++index];
            if (source == "none") {
                args.plaza_credentials_source = Plaza2CredentialSource::None;
            } else if (source == "env") {
                args.plaza_credentials_source = Plaza2CredentialSource::Env;
            } else if (source == "file") {
                args.plaza_credentials_source = Plaza2CredentialSource::File;
            } else {
                std::cerr << "invalid --plaza-credentials-source value\n";
                return std::nullopt;
            }
            continue;
        }
        if (argument == "--plaza-credentials-env-var" && index + 1 < argc) {
            args.plaza_credentials_env_var = argv[++index];
            continue;
        }
        if (argument == "--plaza-credentials-file" && index + 1 < argc) {
            args.plaza_credentials_file = argv[++index];
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
        if (argument == "--armed-test-reconcile") {
            args.armed_test_reconcile = true;
            continue;
        }
        if (argument == "--reconciler-stale-after-steps" && index + 1 < argc) {
            const auto parsed = parse_u64(argv[++index]);
            if (!parsed.has_value()) {
                std::cerr << "invalid --reconciler-stale-after-steps value\n";
                return std::nullopt;
            }
            args.reconciler_stale_after_steps = *parsed;
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
    if (args.twime_endpoint_host.empty() || args.twime_endpoint_port == 0) {
        std::cerr << "--twime-endpoint-host and --twime-endpoint-port are required\n";
        return std::nullopt;
    }
    if (args.plaza_endpoint_host.empty() || args.plaza_endpoint_port == 0) {
        std::cerr << "--plaza-endpoint-host and --plaza-endpoint-port are required\n";
        return std::nullopt;
    }
    return args;
}

std::optional<StreamCode> stream_code_from_name(std::string_view stream_name) {
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

Plaza2TwimeIntegratedTestConfig make_config(const RunnerArgs& args) {
    using namespace moex::plaza2::cgate;
    using namespace moex::twime_trade;

    Plaza2TwimeIntegratedTestConfig config;
    config.profile_id = args.profile_id;
    config.reconciler_stale_after_steps = args.reconciler_stale_after_steps;
    config.arm_state.test_network_armed = args.armed_test_network;
    config.arm_state.test_session_armed = args.armed_test_session;
    config.arm_state.test_plaza2_armed = args.armed_test_plaza2;
    config.arm_state.test_reconcile_armed = args.armed_test_reconcile;

    config.twime.session.session_id = args.profile_id + "_twime";
    config.twime.tcp.environment = TwimeTcpEnvironment::Test;
    config.twime.tcp.endpoint.host = args.twime_endpoint_host;
    config.twime.tcp.endpoint.port = args.twime_endpoint_port;
    config.twime.tcp.endpoint.allow_non_loopback = args.twime_allow_non_loopback;
    config.twime.tcp.endpoint.allow_non_localhost_dns = args.twime_allow_non_localhost_dns;
    config.twime.tcp.test_network_gate.external_test_endpoint_enabled = args.twime_external_test_endpoint_enabled;
    config.twime.tcp.test_network_gate.require_explicit_runtime_arm = args.twime_require_explicit_runtime_arm;
    config.twime.tcp.test_network_gate.block_production_like_hostnames = args.twime_block_production_like_hostnames;
    config.twime.credentials.source = args.twime_credentials_source;
    config.twime.credentials.env_var = args.twime_credentials_env_var;
    config.twime.credentials.file_path = args.twime_credentials_file;
    config.twime.policy.reconnect_enabled = args.twime_reconnect_enabled;
    config.twime.policy.max_reconnect_attempts = args.twime_max_reconnect_attempts;
    config.twime.policy.establish_deadline_ms = args.twime_establish_deadline_ms;
    config.twime.policy.graceful_terminate_timeout_ms = args.twime_graceful_terminate_timeout_ms;

    config.plaza.profile_id = args.profile_id + "_plaza";
    config.plaza.endpoint_host = args.plaza_endpoint_host;
    config.plaza.endpoint_port = args.plaza_endpoint_port;
    config.plaza.runtime.environment = Plaza2Environment::Test;
    config.plaza.runtime.runtime_root = args.plaza_runtime_root;
    config.plaza.runtime.library_path = args.plaza_library_path;
    config.plaza.runtime.scheme_dir = args.plaza_scheme_dir;
    config.plaza.runtime.config_dir = args.plaza_config_dir;
    config.plaza.runtime.env_open_settings = args.plaza_env_open_settings;
    config.plaza.runtime.expected_spectra_release = args.plaza_expected_spectra_release;
    config.plaza.runtime.expected_scheme_sha256 = args.plaza_expected_scheme_sha256;
    config.plaza.connection_settings = args.plaza_connection_settings;
    config.plaza.connection_open_settings = args.plaza_connection_open_settings;
    config.plaza.process_timeout_ms = args.plaza_process_timeout_ms;
    config.plaza.credentials.source = args.plaza_credentials_source;
    config.plaza.credentials.env_var = args.plaza_credentials_env_var;
    config.plaza.credentials.file_path = args.plaza_credentials_file;

    for (const auto& [stream_name, settings] : args.plaza_stream_settings) {
        const auto stream_code = stream_code_from_name(stream_name);
        if (!stream_code.has_value()) {
            throw std::runtime_error("unknown PLAZA II stream name: " + stream_name);
        }
        config.plaza.streams.push_back({
            .stream_code = *stream_code,
            .settings = settings,
        });
    }
    for (const auto& [stream_name, open_settings] : args.plaza_stream_open_settings) {
        const auto stream_code = stream_code_from_name(stream_name);
        if (!stream_code.has_value()) {
            throw std::runtime_error("unknown PLAZA II stream name: " + stream_name);
        }
        for (auto& stream : config.plaza.streams) {
            if (stream.stream_code == *stream_code) {
                stream.open_settings = open_settings;
            }
        }
    }
    return config;
}

void write_startup_report(const fs::path& path, const RunnerArgs& args,
                          const moex::plaza2_twime_reconciler::Plaza2TwimeIntegratedHealthSnapshot& health,
                          bool start_ok, std::string_view start_message) {
    write_summary_json(path,
                       {
                           {"profile_id", args.profile_id},
                           {"result", start_ok ? "started" : "failed"},
                           {"runner_state", runner_state_name(health.state)},
                           {"twime_credentials_source", twime_credential_source_name(args.twime_credentials_source)},
                           {"plaza_credentials_source", plaza_credential_source_name(args.plaza_credentials_source)},
                           {"armed_test_network", bool_string(args.armed_test_network)},
                           {"armed_test_session", bool_string(args.armed_test_session)},
                           {"armed_test_plaza2", bool_string(args.armed_test_plaza2)},
                           {"armed_test_reconcile", bool_string(args.armed_test_reconcile)},
                           {"message", std::string(start_message)},
                       });
}

void write_readiness_report(const fs::path& path,
                            const moex::plaza2_twime_reconciler::Plaza2TwimeIntegratedHealthSnapshot& health) {
    write_summary_json(
        path, {
                  {"runner_state", runner_state_name(health.state)},
                  {"ready", bool_string(health.readiness.ready)},
                  {"readiness_blocker", health.readiness.blocker},
                  {"twime_session_established", bool_string(health.readiness.twime_session_established)},
                  {"plaza_runtime_probe_ok", bool_string(health.readiness.plaza_runtime_probe_ok)},
                  {"plaza_scheme_drift_ok", bool_string(health.readiness.plaza_scheme_drift_ok)},
                  {"plaza_streams_open", bool_string(health.readiness.plaza_streams_open)},
                  {"plaza_streams_online", bool_string(health.readiness.plaza_streams_online)},
                  {"plaza_streams_snapshot_complete", bool_string(health.readiness.plaza_streams_snapshot_complete)},
                  {"reconciler_attached", bool_string(health.readiness.reconciler_attached)},
                  {"abi_snapshot_attached", bool_string(health.readiness.abi_snapshot_attached)},
              });
}

void write_final_report(const fs::path& path,
                        const moex::plaza2_twime_reconciler::Plaza2TwimeIntegratedHealthSnapshot& health,
                        const AbiSnapshotCounts& abi_counts) {
    write_summary_json(
        path, {
                  {"runner_state", runner_state_name(health.state)},
                  {"ready", bool_string(health.readiness.ready)},
                  {"last_error", health.last_error},
                  {"last_resync_reason", health.last_resync_reason},
                  {"plaza_runtime_probe_ok", bool_string(health.plaza_runtime_probe_ok)},
                  {"plaza_scheme_drift_ok", bool_string(health.plaza_scheme_drift_ok)},
                  {"reconciler_updating", bool_string(health.reconciler_updating)},
                  {"abi_handle_valid", bool_string(health.abi_handle_valid)},
                  {"abi_snapshot_attached", bool_string(health.abi_snapshot_attached)},
                  {"session_count", std::to_string(health.plaza.counts.session_count)},
                  {"instrument_count", std::to_string(health.plaza.counts.instrument_count)},
                  {"matching_map_count", std::to_string(health.plaza.counts.matching_map_count)},
                  {"limit_count", std::to_string(health.plaza.counts.limit_count)},
                  {"position_count", std::to_string(health.plaza.counts.position_count)},
                  {"own_order_count", std::to_string(health.plaza.counts.own_order_count)},
                  {"own_trade_count", std::to_string(health.plaza.counts.own_trade_count)},
                  {"reconciled_order_count", std::to_string(health.reconciled_order_count)},
                  {"reconciled_trade_count", std::to_string(health.reconciled_trade_count)},
                  {"abi_private_order_result", std::to_string(abi_counts.private_order_result)},
                  {"abi_private_order_count", std::to_string(abi_counts.private_order_count)},
                  {"abi_private_trade_result", std::to_string(abi_counts.private_trade_result)},
                  {"abi_private_trade_count", std::to_string(abi_counts.private_trade_count)},
                  {"abi_reconciled_order_result", std::to_string(abi_counts.reconciled_order_result)},
                  {"abi_reconciled_order_count", std::to_string(abi_counts.reconciled_order_count)},
                  {"abi_reconciled_trade_result", std::to_string(abi_counts.reconciled_trade_result)},
                  {"abi_reconciled_trade_count", std::to_string(abi_counts.reconciled_trade_count)},
                  {"ambiguity_count",
                   std::to_string(health.reconciler.total_ambiguous_orders + health.reconciler.total_ambiguous_trades)},
                  {"diverged_order_count", std::to_string(health.reconciler.total_diverged_orders)},
                  {"stale_order_count", std::to_string(health.reconciler.total_stale_provisional_orders)},
                  {"plaza_last_lifenum", std::to_string(health.plaza.resume_markers.last_lifenum)},
                  {"plaza_last_replstate", health.plaza.resume_markers.last_replstate},
              });
}

AbiSnapshotCounts capture_abi_counts(MoexConnectorHandle handle) {
    AbiSnapshotCounts counts;
    if (handle == nullptr) {
        return counts;
    }

    counts.private_order_result = moex_get_plaza2_own_order_count(handle, &counts.private_order_count);
    counts.private_trade_result = moex_get_plaza2_own_trade_count(handle, &counts.private_trade_count);
    counts.reconciled_order_result = moex_get_plaza2_reconciled_order_count(handle, &counts.reconciled_order_count);
    counts.reconciled_trade_result = moex_get_plaza2_reconciled_trade_count(handle, &counts.reconciled_trade_count);
    return counts;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto parsed = parse_args(argc, argv);
        if (!parsed.has_value()) {
            return 1;
        }
        const auto args = *parsed;

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        fs::create_directories(args.output_dir);
        const auto startup_path = args.output_dir / (args.profile_id + ".startup.json");
        const auto readiness_path = args.output_dir / (args.profile_id + ".readiness.json");
        const auto final_path = args.output_dir / (args.profile_id + ".final.json");
        const auto operator_log_path = args.output_dir / (args.profile_id + ".operator.log");

        auto runner = moex::plaza2_twime_reconciler::Plaza2TwimeIntegratedTestRunner(make_config(args));
        auto start_result = runner.start();
        write_startup_report(startup_path, args, runner.health_snapshot(), start_result.ok, start_result.message);

        bool readiness_report_written = false;
        std::uint32_t polls = 0;
        auto result = start_result;
        if (result.ok) {
            while (!g_stop_requested.load()) {
                if (args.max_polls != 0 && polls >= args.max_polls) {
                    if (!runner.health_snapshot().readiness.ready) {
                        result = {.ok = false,
                                  .message =
                                      "integrated TEST runner did not reach ready state before --max-polls elapsed"};
                    }
                    break;
                }

                result = runner.poll_once();
                if (!result.ok) {
                    break;
                }
                ++polls;

                if (runner.health_snapshot().readiness.ready && !readiness_report_written) {
                    write_readiness_report(readiness_path, runner.health_snapshot());
                    readiness_report_written = true;
                    if (args.max_polls != 0) {
                        break;
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        if (!readiness_report_written) {
            write_readiness_report(readiness_path, runner.health_snapshot());
        }

        const auto abi_counts = capture_abi_counts(runner.abi_handle());
        const auto stop_result = runner.stop();
        auto final_health = runner.health_snapshot();
        write_lines(operator_log_path, runner.operator_log_lines());
        write_final_report(final_path, final_health, abi_counts);

        if (!result.ok) {
            std::cerr << result.message << '\n';
            return 1;
        }
        if (!stop_result.ok) {
            std::cerr << stop_result.message << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
