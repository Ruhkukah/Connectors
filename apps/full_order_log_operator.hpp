#pragma once

#include "command_socket.hpp"
#include "full_order_log_capture_options.hpp"
#include <csignal>
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
    FullOrderLogCaptureFlags capture_flags;
    bool capture_stop_requested{};
    bool offline{}, live{}, cancel_only{};
    std::string trading_instance, trading_router{"127.0.0.1:4101"};
    std::vector<std::string> args{argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::string_view key = argv[i];
        if (capture_flags.consume(key, i, argc, argv))
            continue;
        if (key == "--offline-fake")
            offline = true;
        else if (key == "--live")
            live = true;
        else if (key == "--cancel-only")
            cancel_only = true;
        else if (key == "--trading-instance-id" || key == "--trading-router") {
            if (++i == argc)
                throw std::invalid_argument("missing FOL operator option value");
            (key == "--trading-instance-id" ? trading_instance : trading_router) = argv[i];
        } else
            args.emplace_back(key);
    }
    capture_flags.validate();
    if (capture_flags.open_interest && (!run || cancel_only))
        throw std::invalid_argument("--open-interest requires FOL run/qualify; not cmd/cancel-only");
    if (!capture_flags.options.directory.empty() && (!run || cancel_only))
        throw std::invalid_argument("capture requires FOL run or qualify; not cmd/cancel-only");
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
    std::unique_ptr<plaza2::cgate::FullOrderLogCapture> capture;
    std::unique_ptr<plaza2::cgate::FullOrderLogOpenInterest> open_interest;
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
                  << " book_valid=" << book->valid() << " rows_total=" << m.rows_total
                  << " lag_samples=" << m.lag_samples << " lag_last_ns=" << m.lag_last_ns
                  << " lag_max_ns=" << m.lag_max_ns << " ignored_executions=" << m.ignored_executions
                  << " clear_deleted_events=" << m.clear_deleted_events
                  << " clear_deleted_erased=" << m.clear_deleted_erased << '\n'
                  << std::flush;
        if (open_interest)
            report_full_order_log_open_interest(*open_interest, book->instruments());
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
        if (!config.clock_offset_us)
            throw std::invalid_argument("FOL run requires measured --clock-offset-us");
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
        session.mode = offline ? plaza2_trade::CgateSessionMode::OfflineFake : plaza2_trade::CgateSessionMode::Live;
        session.aggr20_stream = {};
        session.public_deals_stream = {};
        session.process_timeout_ms = 1;
        session.full_order_log_cancel_only = cancel_only;
        if (cancel_only) {
            session.full_order_log_stream = {};
            session.full_order_log_handler = nullptr;
            session.full_order_log_oi_handler = nullptr;
            session.full_order_log_authority = {};
            config.orders.risk.kill_switch = true;
            return;
        }
        book = std::make_unique<cg::Plaza2FullOrderLog>(config.isin_ids);
        session.collect_poll_metrics = true;
        session.full_order_log_stream = {.stream_code = cg::kFullOrderLogStreamCode,
                                         .settings = cg::full_order_log_listener_settings(session.runtime.scheme_dir)};
        if (!capture_flags.options.directory.empty())
            capture = std::make_unique<cg::FullOrderLogCapture>(capture_flags.options, config.isin_ids, *book);
        session.full_order_log_handler =
            capture ? static_cast<cg::Plaza2ListenerEventHandler*>(capture.get()) : book.get();
        if (capture_flags.open_interest) {
            open_interest = std::make_unique<cg::FullOrderLogOpenInterest>(config.isin_ids);
            open_interest->before_event = [&](const auto& e) {
                return capture ? capture->record(e) : cg::Plaza2Error{};
            };
            open_interest->before_error = [&](const auto& e) {
                if (capture)
                    capture->record_error(moex::plaza2::generated::StreamCode::kFortsDealsRepl, e);
            };
            session.full_order_log_oi_handler = open_interest.get();
        }
        session.full_order_log_authority = [&](std::int32_t isin) {
            return make_full_order_log_trading_authority(*book, isin);
        };
        book->on_crossed = [](std::int32_t isin, bool crossed) {
            std::cerr << "FullOrderLog crossed isin=" << isin << " crossed=" << crossed << '\n';
        };
    };
    hooks.after_poll = [&](CgateTradingHost& host) {
        if (!book)
            return;
        if (capture && !capture_stop_requested) {
            try {
                capture->poll();
            } catch (const std::exception& e) {
                capture_stop_requested = true;
                host.set_kill_switch(true);
                book->reset(true);
                std::cerr << e.what() << "; capture failed, entering normal SIGTERM cancel drain\n";
                // Process-directed signal also reaches the macOS sigwait thread.
                if (::kill(::getpid(), SIGTERM) != 0)
                    throw std::runtime_error("cannot request capture-failure shutdown");
            }
        }
        native.observe(host.last_poll_metrics());
        if (std::chrono::steady_clock::now() - rate_at >= FullOrderLogDtcMetrics::report_interval)
            report();
    };
    hooks.verify_command_endpoint = [](const std::filesystem::path& path) {
        const auto status = send_command(path, "status");
        if (status.find("\"market_data_source\":\"full_order_log\"") == std::string::npos &&
            status.find("\"execution_mode\":\"full_order_log_cancel_only\"") == std::string::npos)
            throw std::invalid_argument("command endpoint is not a Full Order Log trading host");
    };
    std::vector<char*> pointers;
    for (auto& arg : args)
        pointers.push_back(arg.data());
    const auto result = operator_program(static_cast<int>(pointers.size()), pointers.data(), hooks);
    report();
    if (capture) {
        try {
            capture->finish(capture_stop_requested ? plaza2::cgate::FullOrderLogCaptureEndStatus::CaptureFailed
                            : result               ? plaza2::cgate::FullOrderLogCaptureEndStatus::HostNonzero
                                                   : plaza2::cgate::FullOrderLogCaptureEndStatus::Clean,
                            capture_stop_requested ? 3 : result);
        } catch (const std::exception& e) {
            capture_stop_requested = true;
            std::cerr << "capture finalization failed: " << e.what() << '\n';
        }
        report_full_order_log_capture(*capture);
    }
    return capture_stop_requested ? 3 : result;
}
} // namespace moex::connector_host
