#pragma once

#include "command_socket.hpp"
#include "full_order_log_dtc_loop.hpp"
#include "operator_program.hpp"
#include "moex/connector_host/full_order_log_trading.hpp"
#include <iostream>

namespace moex::connector_host {
// The write mode uses the same operator loop and private reconciliation as
// moexctl, with committed FOL data as its explicit market-data authority.
inline int full_order_log_operator(int argc, char** argv,
                                   const std::function<std::string(std::string_view)>& endpoint) {
    const bool run = std::string_view(argv[2]) == "run";
    bool offline{}, live{};
    std::string trading_instance, trading_router{"127.0.0.1:4101"};
    std::vector<std::string> args{argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (key == "--offline-fake")
            offline = true;
        else if (key == "--live")
            live = true;
        else if (key == "--trading-instance-id" || key == "--trading-router") {
            if (++i == argc)
                throw std::invalid_argument("missing FOL operator option value");
            (key == "--trading-instance-id" ? trading_instance : trading_router) = argv[i];
        } else
            args.emplace_back(key);
    }
    const auto has = [&](std::string_view key) { return std::ranges::find(args, key) != args.end(); };
    const auto default_option = [&](const char* key, const char* value) {
        if (!has(key)) {
            args.emplace_back(key);
            args.emplace_back(value);
        }
    };
    default_option("--log", "logs/moex_full_order_log_trading.ndjson");
    if (run) {
        if (offline == live)
            throw std::invalid_argument("FOL run requires --live or --offline-fake");
        if (live && trading_instance.empty())
            throw std::invalid_argument("--live requires the trading process --trading-instance-id");
        if (!has("--allow-orders") || !has("--clock-offset-us"))
            throw std::invalid_argument("FOL run requires --allow-orders and measured --clock-offset-us");
        default_option("--router", "127.0.0.1:4102");
        default_option("--instance-id", "moex_full_order_log");
    }
    std::unique_ptr<plaza2::cgate::Plaza2FullOrderLog> book;
    FullOrderLogDtcMetrics native;
    auto rate_at = std::chrono::steady_clock::now();
    std::uint64_t rows_at{};
    const auto report = [&] {
        if (!book)
            return;
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - rate_at).count();
        const auto m = book->metrics();
        std::cout << "metrics_interval_seconds=" << elapsed << " callbacks=" << native.callbacks
                  << " callbacks_per_second=" << (elapsed > 0 ? native.callbacks / elapsed : 0)
                  << " cg_conn_process_calls=" << native.process_calls
                  << " cg_conn_process_loop_max_ns=" << native.cg_conn_process_loop_max_ns
                  << " rows_per_second=" << (elapsed > 0 ? (m.rows_total - rows_at) / elapsed : 0)
                  << " rows_total=" << m.rows_total << " lag_samples=" << m.lag_samples
                  << " lag_last_ns=" << m.lag_last_ns << " lag_max_ns=" << m.lag_max_ns
                  << " ignored_executions=" << m.ignored_executions
                  << " clear_deleted_events=" << m.clear_deleted_events
                  << " clear_deleted_erased=" << m.clear_deleted_erased << '\n'
                  << std::flush;
        rows_at = m.rows_total;
        rate_at = std::chrono::steady_clock::now();
        native.reset_interval();
    };
    OperatorProgramHooks hooks;
    hooks.configure = [&](TradingHostConfig& config) {
        namespace cg = plaza2::cgate;
        auto& session = config.session;
        if (session.runtime.environment != cg::Plaza2Environment::Test || !session.allow_orders ||
            session.read_only_market_data)
            throw std::invalid_argument("FOL write mode is TEST-only and requires orders explicitly enabled");
        const auto& connection = session.connection_settings;
        if (endpoint(connection.substr(8, connection.find(';') - 8)) == endpoint(trading_router))
            throw std::invalid_argument("order-log and trading router addresses must differ");
        const auto name = connection.substr(connection.find(";app_name=") + 10);
        if (name.substr(0, name.find(';')) == (trading_instance.empty() ? "moex_connector" : trading_instance))
            throw std::invalid_argument("order-log and trading instance IDs must differ");
        if (config.isin_ids.empty() || config.isin_ids.size() > 64 || config.orders.risk.max_quantity > 2 ||
            config.orders.risk.max_open_orders > 5 || config.orders.max_commands_per_second > 5 ||
            std::ranges::any_of(config.orders.risk.max_position_by_isin,
                                [](const auto& item) { return item.second > 2; }))
            throw std::invalid_argument("FOL TEST limits: 64 ISINs, quantity/position 2, open orders/rate 5");
        book = std::make_unique<cg::Plaza2FullOrderLog>(config.isin_ids);
        session.mode = offline ? plaza2_trade::CgateSessionMode::OfflineFake : plaza2_trade::CgateSessionMode::Live;
        session.aggr20_stream = {};
        session.public_deals_stream = {};
        session.process_timeout_ms = 1;
        session.collect_poll_metrics = true;
        session.full_order_log_stream = {.stream_code = cg::kFullOrderLogStreamCode,
                                         .settings = cg::full_order_log_listener_settings(session.runtime.scheme_dir)};
        session.full_order_log_handler = book.get();
        session.full_order_log_authority = [&](std::int32_t isin) {
            return make_full_order_log_trading_authority(*book, isin);
        };
        book->on_crossed = [](std::int32_t isin, bool crossed) {
            std::cerr << "FullOrderLog crossed isin=" << isin << " crossed=" << crossed << '\n';
        };
    };
    hooks.after_poll = [&](CgateTradingHost& host) {
        native.observe(host.last_poll_metrics());
        if (std::chrono::steady_clock::now() - rate_at >= FullOrderLogDtcMetrics::report_interval)
            report();
    };
    hooks.verify_command_endpoint = [](const std::filesystem::path& path) {
        const auto status = send_command(path, "status");
        if (status.find("\"market_data_source\":\"full_order_log\"") == std::string::npos)
            throw std::invalid_argument("command endpoint is not a Full Order Log trading host");
    };
    std::vector<char*> pointers;
    for (auto& arg : args)
        pointers.push_back(arg.data());
    const auto result = operator_program(static_cast<int>(pointers.size()), pointers.data(), hooks);
    report();
    return result;
}
} // namespace moex::connector_host
