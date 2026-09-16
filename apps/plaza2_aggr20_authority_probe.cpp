#include "moex/plaza2/cgate/plaza2_aggr20_authority_probe.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

using moex::plaza2::cgate::Plaza2Aggr20AuthorityProbe;
using moex::plaza2::cgate::Plaza2Aggr20AuthorityProbeConfig;
using moex::plaza2::cgate::Plaza2Aggr20AuthorityProbeReport;
using moex::plaza2::cgate::Plaza2Aggr20AuthorityProbeResult;
using moex::plaza2::cgate::Plaza2CredentialSource;
using moex::plaza2::cgate::Plaza2Environment;

struct ProbeArgs {
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
    std::string refdata_settings{"p2repl://FORTS_REFDATA_REPL"};
    std::string refdata_open_settings{"mode=snapshot+online"};
    std::string aggr_settings{"p2repl://FORTS_AGGR20_REPL"};
    std::string aggr_open_settings{"mode=snapshot+online"};
    std::uint32_t process_timeout_ms{50};
    std::uint32_t observation_seconds{60};
    Plaza2CredentialSource credentials_source{Plaza2CredentialSource::None};
    std::string credentials_env_var;
    fs::path credentials_file;
    Plaza2CredentialSource software_key_source{Plaza2CredentialSource::None};
    std::string software_key_env_var;
    fs::path software_key_file;
    bool armed_test_network{false};
    bool armed_test_session{false};
    bool armed_test_plaza2{false};
};

std::optional<std::uint16_t> parse_port(std::string_view value) {
    std::uint32_t parsed = 0;
    const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || pointer != value.data() + value.size() || parsed > 65535U) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(parsed);
}

std::optional<std::uint32_t> parse_u32(std::string_view value) {
    std::uint32_t parsed = 0;
    const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || pointer != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

bool parse_credential_source(std::string_view value, Plaza2CredentialSource& out) {
    if (value == "none") {
        out = Plaza2CredentialSource::None;
        return true;
    }
    if (value == "env") {
        out = Plaza2CredentialSource::Env;
        return true;
    }
    if (value == "file") {
        out = Plaza2CredentialSource::File;
        return true;
    }
    return false;
}

void usage(std::ostream& out) {
    out << "Usage: moex_plaza2_aggr20_authority_probe [options]\n"
           "  --output-dir DIR --endpoint-host HOST --endpoint-port PORT\n"
           "  --runtime-root DIR --env-open-settings VALUE --connection-settings VALUE\n"
           "  [--connection-open-settings VALUE]\n"
           "  [--refdata-settings VALUE] [--refdata-open-settings VALUE]\n"
           "  [--aggr-settings VALUE] [--aggr-open-settings VALUE]\n"
           "  [--credentials-source none|env|file] [--credentials-env-var NAME]\n"
           "  [--credentials-file PATH] [--software-key-source none|env|file]\n"
           "  [--software-key-env-var NAME] [--software-key-file PATH]\n"
           "  [--observation-seconds N] [--process-timeout-ms N] [--profile-id ID]\n"
           "  [--armed-test-network] [--armed-test-session] [--armed-test-plaza2]\n";
}

std::optional<ProbeArgs> parse_args(int argc, char** argv) {
    ProbeArgs args;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value_required = [&](std::string& target) -> bool {
            if (index + 1 >= argc) {
                std::cerr << argument << " requires a value\n";
                return false;
            }
            target = argv[++index];
            return true;
        };
        if (argument == "--help" || argument == "-h") {
            usage(std::cout);
            return std::nullopt;
        }
        if (argument == "--profile-id") {
            if (!value_required(args.profile_id))
                return std::nullopt;
        } else if (argument == "--output-dir") {
            std::string value;
            if (!value_required(value))
                return std::nullopt;
            args.output_dir = value;
        } else if (argument == "--endpoint-host") {
            if (!value_required(args.endpoint_host))
                return std::nullopt;
        } else if (argument == "--endpoint-port") {
            std::string value;
            if (!value_required(value))
                return std::nullopt;
            const auto parsed = parse_port(value);
            if (!parsed.has_value()) {
                std::cerr << "invalid --endpoint-port value\n";
                return std::nullopt;
            }
            args.endpoint_port = *parsed;
        } else if (argument == "--runtime-root") {
            std::string value;
            if (!value_required(value))
                return std::nullopt;
            args.runtime_root = value;
        } else if (argument == "--library-path") {
            std::string value;
            if (!value_required(value))
                return std::nullopt;
            args.library_path = value;
        } else if (argument == "--scheme-dir") {
            std::string value;
            if (!value_required(value))
                return std::nullopt;
            args.scheme_dir = value;
        } else if (argument == "--config-dir") {
            std::string value;
            if (!value_required(value))
                return std::nullopt;
            args.config_dir = value;
        } else if (argument == "--env-open-settings") {
            if (!value_required(args.env_open_settings))
                return std::nullopt;
        } else if (argument == "--expected-spectra-release") {
            if (!value_required(args.expected_spectra_release))
                return std::nullopt;
        } else if (argument == "--expected-runtime-library-sha256") {
            if (!value_required(args.expected_runtime_library_sha256))
                return std::nullopt;
        } else if (argument == "--expected-scheme-sha256") {
            if (!value_required(args.expected_scheme_sha256))
                return std::nullopt;
        } else if (argument == "--connection-settings") {
            if (!value_required(args.connection_settings))
                return std::nullopt;
        } else if (argument == "--connection-open-settings") {
            if (!value_required(args.connection_open_settings))
                return std::nullopt;
        } else if (argument == "--refdata-settings") {
            if (!value_required(args.refdata_settings))
                return std::nullopt;
        } else if (argument == "--refdata-open-settings") {
            if (!value_required(args.refdata_open_settings))
                return std::nullopt;
        } else if (argument == "--aggr-settings") {
            if (!value_required(args.aggr_settings))
                return std::nullopt;
        } else if (argument == "--aggr-open-settings") {
            if (!value_required(args.aggr_open_settings))
                return std::nullopt;
        } else if (argument == "--process-timeout-ms") {
            std::string value;
            if (!value_required(value))
                return std::nullopt;
            const auto parsed = parse_u32(value);
            if (!parsed.has_value()) {
                std::cerr << "invalid --process-timeout-ms value\n";
                return std::nullopt;
            }
            args.process_timeout_ms = *parsed;
        } else if (argument == "--observation-seconds") {
            std::string value;
            if (!value_required(value))
                return std::nullopt;
            const auto parsed = parse_u32(value);
            if (!parsed.has_value() || *parsed == 0) {
                std::cerr << "--observation-seconds must be positive\n";
                return std::nullopt;
            }
            args.observation_seconds = *parsed;
        } else if (argument == "--credentials-source") {
            std::string value;
            if (!value_required(value) || !parse_credential_source(value, args.credentials_source)) {
                std::cerr << "invalid --credentials-source value\n";
                return std::nullopt;
            }
        } else if (argument == "--credentials-env-var") {
            if (!value_required(args.credentials_env_var))
                return std::nullopt;
        } else if (argument == "--credentials-file") {
            std::string value;
            if (!value_required(value))
                return std::nullopt;
            args.credentials_file = value;
        } else if (argument == "--software-key-source") {
            std::string value;
            if (!value_required(value) || !parse_credential_source(value, args.software_key_source)) {
                std::cerr << "invalid --software-key-source value\n";
                return std::nullopt;
            }
        } else if (argument == "--software-key-env-var") {
            if (!value_required(args.software_key_env_var))
                return std::nullopt;
        } else if (argument == "--software-key-file") {
            std::string value;
            if (!value_required(value))
                return std::nullopt;
            args.software_key_file = value;
        } else if (argument == "--armed-test-network") {
            args.armed_test_network = true;
        } else if (argument == "--armed-test-session") {
            args.armed_test_session = true;
        } else if (argument == "--armed-test-plaza2") {
            args.armed_test_plaza2 = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            return std::nullopt;
        }
    }

    if (args.output_dir.empty() || args.endpoint_host.empty() || args.endpoint_port == 0 || args.runtime_root.empty() ||
        args.env_open_settings.empty() || args.connection_settings.empty()) {
        usage(std::cerr);
        std::cerr << "output, endpoint, runtime, environment, and connection settings are required\n";
        return std::nullopt;
    }
    if (args.profile_id.empty()) {
        args.profile_id = "plaza2_aggr20_authority_probe";
    }
    return args;
}

Plaza2Aggr20AuthorityProbeConfig make_config(const ProbeArgs& args) {
    Plaza2Aggr20AuthorityProbeConfig config;
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
    config.refdata_stream.settings = args.refdata_settings;
    config.refdata_stream.open_settings = args.refdata_open_settings;
    config.aggr_stream.settings = args.aggr_settings;
    config.aggr_stream.open_settings = args.aggr_open_settings;
    config.credentials = {
        .source = args.credentials_source,
        .env_var = args.credentials_env_var,
        .file_path = args.credentials_file,
    };
    config.software_key = {
        .source = args.software_key_source,
        .env_var = args.software_key_env_var,
        .file_path = args.software_key_file,
    };
    config.arm_state.test_network_armed = args.armed_test_network;
    config.arm_state.test_session_armed = args.armed_test_session;
    config.arm_state.test_plaza2_armed = args.armed_test_plaza2;
    config.process_timeout_ms = args.process_timeout_ms;
    config.observation_window = std::chrono::seconds(args.observation_seconds);
    return config;
}

std::string json_escape(std::string_view value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto ch : value) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                out << "\\u" << std::setw(4) << static_cast<unsigned int>(static_cast<unsigned char>(ch));
            } else {
                out << ch;
            }
            break;
        }
    }
    return out.str();
}

void write_string(std::ostream& out, std::string_view value) {
    out << '"' << json_escape(value) << '"';
}

void write_bool(std::ostream& out, bool value) {
    out << (value ? "true" : "false");
}

void write_optional_bool(std::ostream& out, const std::optional<bool>& value) {
    if (!value.has_value()) {
        out << "null";
        return;
    }
    write_bool(out, *value);
}

std::string_view optional_bool_text(const std::optional<bool>& value) {
    if (!value.has_value()) {
        return "unavailable";
    }
    return *value ? "true" : "false";
}

void write_provenance(std::ostream& out,
                      const std::optional<moex::plaza2::private_state::SourceRowProvenance>& provenance) {
    if (!provenance.has_value()) {
        out << "null";
        return;
    }
    out << "{\"stream_code\":" << static_cast<std::uint32_t>(provenance->stream_code)
        << ",\"table_code\":" << static_cast<std::uint32_t>(provenance->table_code)
        << ",\"repl_rev\":" << provenance->repl_rev << ",\"lifenum\":" << provenance->lifenum << ",\"present\":";
    write_bool(out, provenance->present);
    out << '}';
}

void write_report_json(const fs::path& path, const Plaza2Aggr20AuthorityProbeReport& report) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to create probe report: " + path.string());
    }
    out << "{\n  \"result\": ";
    write_string(out, moex::plaza2::cgate::plaza2_aggr20_authority_probe_result_name(report.result));
    out << ",\n  \"MIDSESSION_SESSION_DATA_READY_AFTER_ONLINE\": ";
    write_string(out, moex::plaza2::cgate::plaza2_aggr20_authority_probe_result_name(report.result));
    out << ",\n  \"INITIAL_OPEN_SESSION_DATA_READY_AFTER_ONLINE\": ";
    write_optional_bool(out, report.initial_open_session_data_ready_after_online);
    out << ",\n  \"LISTENER_REOPEN_SESSION_DATA_READY_AFTER_ONLINE\": ";
    write_optional_bool(out, report.listener_reopen_session_data_ready_after_online);
    out << ",\n  \"read_only_contract_ok\": ";
    write_bool(out, report.read_only_contract_ok);
    out << ",\n  \"publisher_create_attempted\": false,\n"
           "  \"publisher_open_attempted\": false,\n"
           "  \"command_api_used\": false,\n"
           "  \"order_api_used\": false,\n"
           "  \"authorization_hash_used\": false,\n"
           "  \"refdata\": {\"listener_created\": ";
    write_bool(out, report.refdata_listener_created);
    out << ",\"listener_opened\": ";
    write_bool(out, report.refdata_listener_opened);
    out << ",\"online\": ";
    write_bool(out, report.refdata_online);
    out << ",\"snapshot_complete\": ";
    write_bool(out, report.refdata_snapshot_complete);
    out << "},\n  \"error\": ";
    write_string(out, report.error);
    out << ",\n  \"selection\": ";
    if (!report.selection.has_value()) {
        out << "null";
    } else {
        const auto& selection = *report.selection;
        out << "{\"sess_id\":" << selection.sess_id << ",\"isin_id\":" << selection.isin_id << ",\"symbol\": ";
        write_string(out, selection.symbol);
        out << ",\"short_symbol\": ";
        write_string(out, selection.short_symbol);
        out << ",\"name\": ";
        write_string(out, selection.name);
        out << ",\"base_contract_code\": ";
        write_string(out, selection.base_contract_code);
        out << ",\"refdata_lifenum\":" << selection.refdata_lifenum << ",\"fut_instruments_source\":";
        write_provenance(out, selection.fut_instruments_source);
        out << ",\"fut_sess_contents_source\":";
        write_provenance(out, selection.fut_sess_contents_source);
        out << ",\"session_source\":";
        write_provenance(out, selection.session_source);
        out << '}';
    }
    out << ",\n  \"attempts\": [\n";
    for (std::size_t attempt_index = 0; attempt_index < report.attempts.size(); ++attempt_index) {
        const auto& attempt = report.attempts[attempt_index];
        out << "    {\"ordinal\":" << attempt.ordinal << ",\"name\": ";
        write_string(out, attempt.name);
        out << ",\"listener_created\":";
        write_bool(out, attempt.listener_created);
        out << ",\"listener_opened\":";
        write_bool(out, attempt.listener_opened);
        out << ",\"online\":";
        write_bool(out, attempt.online);
        out << ",\"snapshot_complete\":";
        write_bool(out, attempt.snapshot_complete);
        out << ",\"session_data_ready_after_online\":";
        write_bool(out, attempt.session_data_ready_after_online);
        out << ",\"target_authoritative\":";
        write_bool(out, attempt.target_authoritative);
        out << ",\"experiment_complete\":";
        write_bool(out, attempt.experiment_complete);
        out << ",\"error\": ";
        write_string(out, attempt.error);
        out << ",\"sys_events\":[\n";
        for (std::size_t event_index = 0; event_index < attempt.sys_events.size(); ++event_index) {
            const auto& event = attempt.sys_events[event_index];
            out << "      {\"source_repl_id\":" << event.source_repl_id
                << ",\"source_repl_rev\":" << event.source_repl_rev << ",\"source_repl_act\":" << event.source_repl_act
                << ",\"event_id\":" << event.event_id << ",\"event_type\":" << event.event_type
                << ",\"sess_id\":" << event.sess_id << ",\"message\": ";
            write_string(out, event.message);
            out << ",\"server_time\":" << event.server_time << ",\"transaction_id\":" << event.transaction_id
                << ",\"transaction_row_index\":" << event.transaction_row_index << ",\"observed_before_online\":";
            write_bool(out, event.observed_before_online);
            out << ",\"transaction_committed\":";
            write_bool(out, event.transaction_committed);
            out << "}" << (event_index + 1 == attempt.sys_events.size() ? "\n" : ",\n");
        }
        out << "    ],\"event_trace\":[\n";
        for (std::size_t trace_index = 0; trace_index < attempt.event_trace.size(); ++trace_index) {
            out << "      ";
            write_string(out, attempt.event_trace[trace_index]);
            out << (trace_index + 1 == attempt.event_trace.size() ? "\n" : ",\n");
        }
        out << "    ]}" << (attempt_index + 1 == report.attempts.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
}

void write_report_log(const fs::path& path, const Plaza2Aggr20AuthorityProbeReport& report) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("failed to create probe log: " + path.string());
    }
    out << "MIDSESSION_SESSION_DATA_READY_AFTER_ONLINE = "
        << moex::plaza2::cgate::plaza2_aggr20_authority_probe_result_name(report.result) << '\n';
    out << "INITIAL_OPEN_SESSION_DATA_READY_AFTER_ONLINE = "
        << optional_bool_text(report.initial_open_session_data_ready_after_online) << '\n';
    out << "LISTENER_REOPEN_SESSION_DATA_READY_AFTER_ONLINE = "
        << optional_bool_text(report.listener_reopen_session_data_ready_after_online) << '\n';
    out << "read_only_contract_ok=" << (report.read_only_contract_ok ? "true" : "false") << '\n';
    out << "publisher_create_attempted=false\npublisher_open_attempted=false\ncommand_api_used=false\n"
           "order_api_used=false\nauthorization_hash_used=false\n";
    if (!report.error.empty()) {
        out << "error=" << report.error << '\n';
    }
    if (report.selection.has_value()) {
        const auto& selection = *report.selection;
        out << "selected_sess_id=" << selection.sess_id << " selected_isin_id=" << selection.isin_id
            << " selected_symbol=" << selection.symbol << " selected_short_symbol=" << selection.short_symbol
            << " refdata_lifenum=" << selection.refdata_lifenum << '\n';
    }
    for (const auto& attempt : report.attempts) {
        out << "attempt=" << attempt.ordinal << " name=" << attempt.name
            << " listener_created=" << (attempt.listener_created ? "true" : "false")
            << " listener_opened=" << (attempt.listener_opened ? "true" : "false")
            << " online=" << (attempt.online ? "true" : "false")
            << " snapshot_complete=" << (attempt.snapshot_complete ? "true" : "false")
            << " session_data_ready_after_online=" << (attempt.session_data_ready_after_online ? "true" : "false")
            << " target_authoritative=" << (attempt.target_authoritative ? "true" : "false")
            << " experiment_complete=" << (attempt.experiment_complete ? "true" : "false") << '\n';
        for (const auto& event : attempt.sys_events) {
            out << "sys_event attempt=" << attempt.ordinal << " transaction_id=" << event.transaction_id
                << " transaction_row_index=" << event.transaction_row_index
                << " transaction_committed=" << (event.transaction_committed ? "true" : "false")
                << " observed_before_online=" << (event.observed_before_online ? "true" : "false")
                << " replID=" << event.source_repl_id << " replRev=" << event.source_repl_rev
                << " event_id=" << event.event_id << " event_type=" << event.event_type << " sess_id=" << event.sess_id
                << " message=" << event.message << " server_time=" << event.server_time << '\n';
        }
        for (const auto& line : attempt.event_trace) {
            out << line << '\n';
        }
        if (!attempt.error.empty()) {
            out << "attempt_error=" << attempt.error << '\n';
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto parsed = parse_args(argc, argv);
        if (!parsed.has_value()) {
            return 1;
        }
        const auto args = *parsed;
        fs::create_directories(args.output_dir);
        Plaza2Aggr20AuthorityProbe probe(make_config(args));
        const auto report = probe.run();
        write_report_json(args.output_dir / "aggr20_authority_probe.json", report);
        write_report_log(args.output_dir / "aggr20_authority_probe.log", report);
        if (!report.error.empty()) {
            std::cerr << report.error << '\n';
        }
        return report.result == Plaza2Aggr20AuthorityProbeResult::Inconclusive ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
