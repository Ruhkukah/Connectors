#include "harness.hpp"
#include "moex/connector_host/operator_config.hpp"
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace moex::connector_host;
namespace {
std::string env(const std::string& name) {
    const auto* value = std::getenv(name.c_str());
    if (!value || !*value)
        throw std::invalid_argument("missing environment input");
    return value;
}
std::int64_t number(const std::string& text) {
    std::int64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value <= 0)
        throw std::invalid_argument("invalid positive integer");
    return value;
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Isolated TEST-only no-post host harness, not a CLI signal or certification result.\n"
                     "Requires --execute-isolated-test plus value flags: --case "
                     "healthy|recovered-empty|unavailable|second-loss,\n"
                     "--runtime-root --library --scheme-dir --config-dir --env-open-var --credentials-env "
                     "--software-key-env\n"
                     "--broker --client --login-env --ext-id-begin --ext-id-end --identity-state --journal --isin "
                     "--quote-cap-scaled.\n"
                     "Uses only TEST router 127.0.0.1:4101. No timer, router, upstream or order controls.\n";
        return 0;
    }
    try {
        std::map<std::string, std::string> args;
        bool execute{};
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--execute-isolated-test") {
                if (execute)
                    throw std::invalid_argument("duplicate execute flag");
                execute = true;
            } else if (i + 1 == argc || !args.emplace(key, argv[++i]).second)
                throw std::invalid_argument("invalid arguments");
        }
        const std::string keys[] = {
            "--case",         "--runtime-root",    "--library",          "--scheme-dir",     "--config-dir",
            "--env-open-var", "--credentials-env", "--software-key-env", "--broker",         "--client",
            "--login-env",    "--ext-id-begin",    "--ext-id-end",       "--identity-state", "--journal",
            "--isin",         "--quote-cap-scaled"};
        if (!execute || args.size() != std::size(keys))
            throw std::invalid_argument("explicit execution inputs required");
        for (const auto& key : keys)
            if (!args.contains(key) || args.at(key).empty())
                throw std::invalid_argument("missing argument");
        const auto scenario = args.at("--case");
        ShutdownCase kind;
        if (scenario == "healthy")
            kind = ShutdownCase::Healthy;
        else if (scenario == "recovered-empty")
            kind = ShutdownCase::RecoveredEmpty;
        else if (scenario == "unavailable")
            kind = ShutdownCase::Unavailable;
        else if (scenario == "second-loss")
            kind = ShutdownCase::SecondLoss;
        else
            throw std::invalid_argument("unknown case");
        const auto isin = number(args.at("--isin"));
        const auto begin = number(args.at("--ext-id-begin")), end = number(args.at("--ext-id-end"));
        if (isin > INT32_MAX || begin >= INT32_MAX || end >= INT32_MAX || end < begin)
            throw std::invalid_argument("invalid identity bounds");
        Plaza2HostConfigInputs input;
        input.runtime_root = args.at("--runtime-root");
        input.library_path = args.at("--library");
        input.scheme_dir = args.at("--scheme-dir");
        input.config_dir = args.at("--config-dir");
        input.env_open_settings = env(args.at("--env-open-var"));
        input.credentials_env_var = args.at("--credentials-env");
        input.software_key_env_var = args.at("--software-key-env");
        input.broker_code = args.at("--broker");
        input.client_code = args.at("--client");
        input.isin_ids = {isin};
        input.allow_orders = true;
        input.publisher_messages_per_second = 5;
        TradingHostConfig config;
        config.session = build_plaza2_host_config(input).transport.host;
        config.session.mode = moex::plaza2_trade::CgateSessionMode::Live;
        config.session.trade_replay_from_pos_anchor = true;
        config.isin_ids = {static_cast<std::int32_t>(isin)};
        config.journal_path = args.at("--journal");
        config.identity_state_path = args.at("--identity-state");
        config.source_git_sha = MOEX_SOURCE_GIT_SHA;
        config.orders.broker_code = input.broker_code;
        config.orders.client_code = input.client_code;
        config.orders.login_from = env(args.at("--login-env"));
        config.orders.ext_id_begin = begin;
        config.orders.ext_id_end = end;
        config.orders.ext_id_range_configured = true;
        config.orders.sole_instance = true;
        config.orders.max_commands_per_second = 5;
        config.orders.command_rate_configured = true;
        config.orders.risk.max_quantity = 2;
        config.orders.risk.quantity_configured = true;
        config.orders.risk.max_open_orders = 5;
        config.orders.risk.open_orders_configured = true;
        config.orders.risk.max_notional_by_isin[isin] = number(args.at("--quote-cap-scaled"));
        config.orders.risk.max_position_by_isin[isin] = 2;
        const auto result = ShutdownReconnectHarness::run(std::move(config), kind);
        const auto b = [](bool value) { return value ? "true" : "false"; };
        std::cout << "{\"schema\":1,\"case\":\"" << scenario << "\",\"source_git_sha\":\"" << MOEX_SOURCE_GIT_SHA
                  << "\",\"scope\":\"isolated_host_not_cli_signal\",\"publisher_posts_disabled\":true,\"admitted\":"
                  << b(result.admitted) << ",\"loss_observed\":" << b(result.loss_observed)
                  << ",\"deferred_observed\":" << b(result.deferred_observed)
                  << ",\"recovered\":" << b(result.recovered)
                  << ",\"second_loss_observed\":" << b(result.second_loss_observed)
                  << ",\"timed_out\":" << b(result.timed_out)
                  << ",\"reconciliation_pending\":" << b(result.reconciliation_pending)
                  << ",\"pending_cancellations\":" << b(result.pending_cancellations)
                  << ",\"working_orders\":" << b(result.working_orders) << ",\"stop_ok\":" << b(result.stop_ok)
                  << ",\"shutdown_exit\":" << result.shutdown_exit << ",\"failure\":\"" << result.failure << "\"}\n";
        return result.failure.empty() ? result.shutdown_exit : 2;
    } catch (...) {
        std::cout << "{\"schema\":1,\"failure\":\"invalid_input_or_host_construction\",\"native_result\":false}\n";
        return 2;
    }
}
