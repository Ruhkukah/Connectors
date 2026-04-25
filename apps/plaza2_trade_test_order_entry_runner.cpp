#include "moex/plaza2_trade/plaza2_trade_test_order_runner.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using moex::plaza2::cgate::Plaza2CredentialSource;
using moex::plaza2::cgate::Plaza2Environment;
using moex::plaza2::cgate::Plaza2LiveStreamConfig;
using moex::plaza2::generated::StreamCode;
using moex::plaza2_trade::plaza2_trade_order_entry_failure_name;
using moex::plaza2_trade::Plaza2TradeOrderEntryConfig;
using moex::plaza2_trade::Plaza2TradeOrderEntryMode;
using moex::plaza2_trade::Plaza2TradeSide;
using moex::plaza2_trade::Plaza2TradeTestOrderRunner;

struct RunnerArgs {
    Plaza2TradeOrderEntryConfig config;
    fs::path output_dir;
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

void write_json(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& fields) {
    std::ofstream out(path);
    out << "{\n";
    for (std::size_t index = 0; index < fields.size(); ++index) {
        out << "  \"" << escape_json(fields[index].first) << "\": \"" << escape_json(fields[index].second) << "\"";
        out << (index + 1 == fields.size() ? "\n" : ",\n");
    }
    out << "}\n";
}

std::optional<std::uint32_t> parse_u32(std::string_view value) {
    try {
        return static_cast<std::uint32_t>(std::stoul(std::string(value)));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::int32_t> parse_i32(std::string_view value) {
    try {
        return static_cast<std::int32_t>(std::stol(std::string(value)));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::uint16_t> parse_port(std::string_view value) {
    const auto parsed = parse_u32(value);
    if (!parsed.has_value() || *parsed > 65535U) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(*parsed);
}

std::optional<Plaza2CredentialSource> parse_credential_source(std::string_view value) {
    if (value == "none") {
        return Plaza2CredentialSource::None;
    }
    if (value == "env") {
        return Plaza2CredentialSource::Env;
    }
    if (value == "file") {
        return Plaza2CredentialSource::File;
    }
    return std::nullopt;
}

std::optional<Plaza2TradeSide> parse_side(std::string_view value) {
    if (value == "buy" || value == "Buy" || value == "1") {
        return Plaza2TradeSide::Buy;
    }
    if (value == "sell" || value == "Sell" || value == "2") {
        return Plaza2TradeSide::Sell;
    }
    return std::nullopt;
}

void set_stream_settings(Plaza2TradeOrderEntryConfig& config, StreamCode code, std::string value) {
    for (auto& stream : config.private_session.streams) {
        if (stream.stream_code == code) {
            stream.settings = std::move(value);
            return;
        }
    }
    config.private_session.streams.push_back({
        .stream_code = code,
        .settings = std::move(value),
    });
}

std::optional<RunnerArgs> parse_args(int argc, char** argv) {
    RunnerArgs args;
    auto& config = args.config;
    config.private_session.runtime.environment = Plaza2Environment::Test;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto require_value = [&](const char* name) -> std::optional<std::string> {
            if (index + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                return std::nullopt;
            }
            return std::string(argv[++index]);
        };

        if (argument == "--profile-id") {
            if (auto value = require_value("--profile-id")) {
                config.profile_id = *value;
                config.private_session.profile_id = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--output-dir") {
            if (auto value = require_value("--output-dir")) {
                args.output_dir = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--runtime-root") {
            if (auto value = require_value("--runtime-root")) {
                config.private_session.runtime.runtime_root = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--library-path") {
            if (auto value = require_value("--library-path")) {
                config.private_session.runtime.library_path = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--scheme-dir") {
            if (auto value = require_value("--scheme-dir")) {
                config.private_session.runtime.scheme_dir = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--config-dir") {
            if (auto value = require_value("--config-dir")) {
                config.private_session.runtime.config_dir = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--env-open-settings") {
            if (auto value = require_value("--env-open-settings")) {
                config.private_session.runtime.env_open_settings = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--connection-settings") {
            if (auto value = require_value("--connection-settings")) {
                config.private_session.connection_settings = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--publisher-settings") {
            if (auto value = require_value("--publisher-settings")) {
                config.publisher.settings = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--publisher-open-settings") {
            if (auto value = require_value("--publisher-open-settings")) {
                config.publisher.open_settings = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--endpoint-host") {
            if (auto value = require_value("--endpoint-host")) {
                config.private_session.endpoint_host = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--endpoint-port") {
            const auto value = require_value("--endpoint-port");
            const auto parsed = value.has_value() ? parse_port(*value) : std::nullopt;
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            config.private_session.endpoint_port = *parsed;
        } else if (argument == "--expected-spectra-release") {
            if (auto value = require_value("--expected-spectra-release")) {
                config.private_session.runtime.expected_spectra_release = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--expected-scheme-sha256") {
            if (auto value = require_value("--expected-scheme-sha256")) {
                config.private_session.runtime.expected_scheme_sha256 = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--credentials-source") {
            const auto value = require_value("--credentials-source");
            const auto parsed = value.has_value() ? parse_credential_source(*value) : std::nullopt;
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            config.private_session.credentials.source = *parsed;
        } else if (argument == "--credentials-env-var") {
            if (auto value = require_value("--credentials-env-var")) {
                config.private_session.credentials.env_var = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--software-key-source") {
            const auto value = require_value("--software-key-source");
            const auto parsed = value.has_value() ? parse_credential_source(*value) : std::nullopt;
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            config.private_session.software_key.source = *parsed;
        } else if (argument == "--software-key-env-var") {
            if (auto value = require_value("--software-key-env-var")) {
                config.private_session.software_key.env_var = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--trade-stream-settings") {
            if (auto value = require_value("--trade-stream-settings")) {
                set_stream_settings(config, StreamCode::kFortsTradeRepl, *value);
            } else {
                return std::nullopt;
            }
        } else if (argument == "--userorderbook-stream-settings") {
            if (auto value = require_value("--userorderbook-stream-settings")) {
                set_stream_settings(config, StreamCode::kFortsUserorderbookRepl, *value);
            } else {
                return std::nullopt;
            }
        } else if (argument == "--pos-stream-settings") {
            if (auto value = require_value("--pos-stream-settings")) {
                set_stream_settings(config, StreamCode::kFortsPosRepl, *value);
            } else {
                return std::nullopt;
            }
        } else if (argument == "--part-stream-settings") {
            if (auto value = require_value("--part-stream-settings")) {
                set_stream_settings(config, StreamCode::kFortsPartRepl, *value);
            } else {
                return std::nullopt;
            }
        } else if (argument == "--refdata-stream-settings") {
            if (auto value = require_value("--refdata-stream-settings")) {
                set_stream_settings(config, StreamCode::kFortsRefdataRepl, *value);
            } else {
                return std::nullopt;
            }
        } else if (argument == "--stream-open-settings") {
            if (auto value = require_value("--stream-open-settings")) {
                for (auto& stream : config.private_session.streams) {
                    stream.open_settings = *value;
                }
            } else {
                return std::nullopt;
            }
        } else if (argument == "--isin-id") {
            const auto value = require_value("--isin-id");
            const auto parsed = value.has_value() ? parse_i32(*value) : std::nullopt;
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            config.tiny_order.isin_id = *parsed;
        } else if (argument == "--broker-code") {
            if (auto value = require_value("--broker-code")) {
                config.tiny_order.broker_code = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--client-code") {
            if (auto value = require_value("--client-code")) {
                config.tiny_order.client_code = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--side") {
            const auto value = require_value("--side");
            const auto parsed = value.has_value() ? parse_side(*value) : std::nullopt;
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            config.tiny_order.side = *parsed;
        } else if (argument == "--price") {
            if (auto value = require_value("--price")) {
                config.tiny_order.price = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--quantity") {
            const auto value = require_value("--quantity");
            const auto parsed = value.has_value() ? parse_i32(*value) : std::nullopt;
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            config.tiny_order.quantity = *parsed;
        } else if (argument == "--max-quantity") {
            const auto value = require_value("--max-quantity");
            const auto parsed = value.has_value() ? parse_i32(*value) : std::nullopt;
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            config.tiny_order.max_quantity = *parsed;
        } else if (argument == "--ext-id") {
            const auto value = require_value("--ext-id");
            const auto parsed = value.has_value() ? parse_i32(*value) : std::nullopt;
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            config.tiny_order.ext_id = *parsed;
        } else if (argument == "--client-transaction-id-prefix") {
            if (auto value = require_value("--client-transaction-id-prefix")) {
                config.tiny_order.client_transaction_id_prefix = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--comment") {
            if (auto value = require_value("--comment")) {
                config.tiny_order.comment = *value;
            } else {
                return std::nullopt;
            }
        } else if (argument == "--max-polls") {
            const auto value = require_value("--max-polls");
            const auto parsed = value.has_value() ? parse_u32(*value) : std::nullopt;
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            config.max_polls = *parsed;
        } else if (argument == "--armed-test-network") {
            config.arm_state.test_network_armed = true;
            config.private_session.arm_state.test_network_armed = true;
        } else if (argument == "--armed-test-session") {
            config.arm_state.test_session_armed = true;
            config.private_session.arm_state.test_session_armed = true;
        } else if (argument == "--armed-test-plaza2") {
            config.arm_state.test_plaza2_armed = true;
            config.private_session.arm_state.test_plaza2_armed = true;
        } else if (argument == "--armed-test-order-entry") {
            config.order_entry_armed = true;
        } else if (argument == "--armed-test-tiny-order") {
            config.tiny_order_armed = true;
        } else if (argument == "--send-test-order") {
            config.mode = Plaza2TradeOrderEntryMode::SendTestOrder;
            config.send_test_order = true;
        } else if (argument == "--dry-run") {
            config.mode = Plaza2TradeOrderEntryMode::DryRun;
            config.send_test_order = false;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            return std::nullopt;
        }
    }
    if (config.profile_id.empty() || args.output_dir.empty()) {
        std::cerr << "--profile-id and --output-dir are required\n";
        return std::nullopt;
    }
    return args;
}

void write_evidence(const fs::path& output_dir, const Plaza2TradeOrderEntryConfig& config,
                    const moex::plaza2_trade::Plaza2TradeOrderEntryResult& result) {
    fs::create_directories(output_dir);
    write_json(output_dir / "startup.json",
               {
                   {"profile_id", config.profile_id},
                   {"mode", config.mode == Plaza2TradeOrderEntryMode::DryRun ? "dry_run" : "send_test_order"},
               });
    write_json(output_dir / "runtime_probe.json", {{"runtime_probe_ok", result.ok ? "not_run_or_passed" : "unknown"}});
    write_json(output_dir / "scheme_drift.json", {{"scheme_drift_ok", result.ok ? "not_run_or_passed" : "unknown"}});
    write_json(output_dir / "order_entry_plan.json",
               {
                   {"command", "AddOrder"},
                   {"cancel_command", "DelOrder"},
                   {"live_enabled_commands", "AddOrder,DelOrder"},
                   {"deferred_commands", "IcebergAddOrder,IcebergDelOrder,MoveOrder,IcebergMoveOrder,DelUserOrders,"
                                         "DelOrdersByBFLimit,CODHeartbeat"},
               });
    write_json(output_dir / "pre_send_summary.json",
               {
                   {"instrument_isin_id", std::to_string(config.tiny_order.isin_id)},
                   {"side", config.tiny_order.side == Plaza2TradeSide::Buy ? "buy" : "sell"},
                   {"price", config.tiny_order.price},
                   {"quantity", std::to_string(config.tiny_order.quantity)},
                   {"client_code", "[REDACTED]"},
                   {"broker_code", "[REDACTED]"},
               });
    write_json(output_dir / "add_order_reply.json",
               {
                   {"reply_received", result.evidence.reply_received ? "true" : "false"},
                   {"reply_accepted", result.evidence.reply_accepted ? "true" : "false"},
               });
    write_json(output_dir / "add_order_confirmation.json",
               {
                   {"private_order_seen", result.evidence.private_order_seen ? "true" : "false"},
                   {"user_orderbook_seen", result.evidence.user_orderbook_seen ? "true" : "false"},
               });
    write_json(output_dir / "cancel_order_reply.json",
               {{"cancel_submitted", result.evidence.cancel_submitted ? "true" : "false"}});
    write_json(output_dir / "cancel_order_confirmation.json",
               {{"cancel_confirmed", result.evidence.cancel_confirmed ? "true" : "false"}});
    write_json(output_dir / "final_health.json",
               {
                   {"ok", result.ok ? "true" : "false"},
                   {"failure_classification", std::string(plaza2_trade_order_entry_failure_name(result.failure))},
                   {"message", result.message},
                   {"command_encoded", result.evidence.command_encoded ? "true" : "false"},
                   {"command_submitted", result.evidence.command_submitted ? "true" : "false"},
               });
    write_json(output_dir / "run_manifest.json", {
                                                     {"runner", "moex_plaza2_trade_test_order_entry_runner"},
                                                     {"scope", "phase5e_test_only_add_del_order"},
                                                     {"secrets", "redacted"},
                                                 });
    std::ofstream log(output_dir / "operator.log");
    log << "profile=" << config.profile_id << '\n';
    log << "mode=" << (config.mode == Plaza2TradeOrderEntryMode::DryRun ? "dry_run" : "send_test_order") << '\n';
    log << "result=" << result.message << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const auto args = parse_args(argc, argv);
    if (!args.has_value()) {
        return EXIT_FAILURE;
    }

    std::unique_ptr<moex::plaza2_trade::Plaza2TradeOrderEntryGateway> gateway;
    if (args->config.mode == Plaza2TradeOrderEntryMode::SendTestOrder) {
        gateway = std::make_unique<moex::plaza2_trade::Plaza2TradeLiveOrderEntryGateway>(args->config);
    }
    Plaza2TradeTestOrderRunner runner(args->config, std::move(gateway));
    const auto result = runner.run();
    write_evidence(args->output_dir, args->config, result);
    if (!result.ok) {
        std::cerr << result.message << '\n';
        return EXIT_FAILURE;
    }
    std::cout << result.message << '\n';
    return EXIT_SUCCESS;
}
