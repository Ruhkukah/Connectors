#include "full_order_log_dtc_loop.hpp"
#include "moex/connector_host/operator_config.hpp"
#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>

namespace {
namespace dtc = moex::connector_host::dtc;
namespace cg = moex::plaza2::cgate;
std::atomic_bool stop_requested{};
void stop_signal(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}
std::uint32_t integer(std::string_view value) {
    std::uint32_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !result)
        throw std::invalid_argument("expected positive integer");
    return result;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "moex_full_order_log_dtc_runner plaza2 qualify [read-only runtime options]\n"
                         "  --dtc-port N (default 11300, consecutive loopback ports for configured ISINs)\n"
                         "  --live --trading-instance-id NAME (MOEX/broker entitlement is the owner responsibility)\n"
                         "  --router HOST:PORT (default 127.0.0.1:4102; separate order-log router/login)\n"
                         "  --offline-fake --run-ms N (offline tests; no live authorization inferred)\n"
                         "  --dtc-depth N (default 20; up to 20000 per side; NumLevels=0 uses that cap)\n"
                         "Each port serves one configured ISIN from the full anonymous book.\n"
                         "Order entry, accounts, positions and market-by-order are unavailable.\n"
                      << moex::connector_host::operator_help();
            return 0;
        }
        bool offline = false, live = false, instance = false, router = false;
        std::string trading_instance;
        std::uint32_t base_port = 11300, run_ms = 0, depth_levels = 20;
        std::vector<std::string_view> arguments;
        for (int i = 1; i < argc; ++i) {
            const std::string_view key = argv[i];
            if (key == "--offline-fake")
                offline = true;
            else if (key == "--live")
                live = true;
            else if (key == "--trading-instance-id" || key == "--dtc-port" || key == "--run-ms" ||
                     key == "--dtc-depth") {
                if (++i == argc)
                    throw std::invalid_argument("missing runner option value");
                if (key == "--trading-instance-id")
                    trading_instance = argv[i];
                else if (key == "--dtc-port")
                    base_port = integer(argv[i]);
                else if (key == "--dtc-depth")
                    depth_levels = integer(argv[i]);
                else
                    run_ms = integer(argv[i]);
            } else {
                instance |= key == "--instance-id";
                router |= key == "--router";
                arguments.push_back(key);
            }
        }
        if (offline == live)
            throw std::invalid_argument("select --live or --offline-fake");
        if (live && trading_instance.empty())
            throw std::invalid_argument("--live requires the trading process --trading-instance-id");
        if (!router) {
            arguments.push_back("--router");
            arguments.push_back("127.0.0.1:4102");
        }
        arguments.push_back("--read-only-market-data");
        if (!instance) {
            arguments.push_back("--instance-id");
            arguments.push_back("moex_full_order_log");
        }
        auto request = moex::connector_host::parse_operator_arguments(arguments);
        const auto app_name = request.config.transport.host.connection_settings.substr(
            request.config.transport.host.connection_settings.find(";app_name=") + 10);
        if (app_name.substr(0, app_name.find(';')) == (trading_instance.empty() ? "moex_connector" : trading_instance))
            throw std::invalid_argument("order-log and trading instance IDs must differ");
        if (request.command != "qualify")
            throw std::invalid_argument("FullOrderLog runner requires plaza2 qualify");
        if (request.config.isin_ids.size() > 64 || depth_levels > 20000 || base_port > UINT16_MAX ||
            request.config.isin_ids.size() - 1 > UINT16_MAX - base_port)
            throw std::invalid_argument("invalid bounded ISIN universe or DTC port range");
        std::vector<std::int32_t> ids;
        for (const auto isin : request.config.isin_ids)
            ids.push_back(static_cast<std::int32_t>(isin));
        cg::Plaza2FullOrderLog book(ids);
        auto& transport = request.config.transport.host;
        transport.mode =
            offline ? moex::plaza2_trade::CgateSessionMode::OfflineFake : moex::plaza2_trade::CgateSessionMode::Live;
        transport.aggr20_stream = {};
        transport.public_deals_stream = {};
        transport.process_timeout_ms = 1;
        transport.full_order_log_stream = {.stream_code = cg::kFullOrderLogStreamCode,
                                           .settings = "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL",
                                           .open_settings = ""};
        transport.full_order_log_handler = &book;
        transport.event_log = [](std::string_view event, std::string_view detail) {
            std::cerr << event << ' ' << detail << '\n';
        };
        transport.listener_event_log = [](const cg::Plaza2ListenerEvent& event) {
            if (event.stream_code != cg::kFullOrderLogStreamCode)
                return;
            using Kind = cg::Plaza2ListenerEventKind;
            if (event.kind == Kind::Open || event.kind == Kind::Close || event.kind == Kind::Online ||
                event.kind == Kind::LifeNum || event.kind == Kind::ClearDeleted ||
                (event.kind == Kind::StreamData && event.message_name == "sys_events"))
                std::cerr << "FullOrderLog listener event " << static_cast<unsigned>(event.kind)
                          << " life=" << event.unsigned_value << " revision=" << event.signed_value
                          << " table=" << (event.message_name.empty() ? "unspecified" : event.message_name) << '\n';
        };
        moex::connector_host::ConnectorHost host(std::move(request.config));
        moex::connector_host::FullOrderLogDtcLoop loop(host, book, static_cast<std::uint16_t>(base_port), depth_levels,
                                                       offline ? dtc::DtcSourceMode::Replay
                                                               : dtc::DtcSourceMode::LiveTest);
        book.on_crossed = [](std::int32_t isin, bool crossed) {
            std::cerr << "FullOrderLog crossed isin=" << isin << " crossed=" << crossed << '\n';
        };
        std::uint64_t logged_info_life = UINT64_MAX;
        loop.on_queued_commit = [&] {
            const auto life = book.life_state();
            if (life.info_available && logged_info_life != life.info_trades_lifenum) {
                logged_info_life = life.info_trades_lifenum;
                std::cerr << "FullOrderLog committed info.trades_lifenum=" << logged_info_life << '\n';
            }
        };
        std::signal(SIGINT, stop_signal);
        std::signal(SIGTERM, stop_signal);
        loop.start();
        for (std::size_t i = 0; i < ids.size(); ++i)
            std::cout << "isin=" << book.instruments()[i] << " dtc_port=" << loop.server(i).port()
                      << " symbol_id=" << i + 1 << " source_mode=" << (offline ? "offline_fake" : "live_test") << '\n';
        std::cout << "book_memory_bytes=" << book.memory_bytes()
                  << " max_queue_bytes_per_isin=4194304 dtc_depth_per_side=" << depth_levels << '\n'
                  << std::flush;
        const auto start = std::chrono::steady_clock::now();
        auto rate_at = start;
        std::uint64_t rows_at = book.metrics().rows_total;
        while (!stop_requested.load(std::memory_order_relaxed) &&
               (!run_ms || std::chrono::steady_clock::now() - start < std::chrono::milliseconds(run_ms))) {
            loop.poll();
            const auto now = std::chrono::steady_clock::now();
            if (now - rate_at >= std::chrono::seconds(30)) {
                const auto m = book.metrics();
                std::cout << "rows_per_second="
                          << (m.rows_total - rows_at) / std::chrono::duration<double>(now - rate_at).count()
                          << " lag_samples=" << m.lag_samples << " lag_last_ns=" << m.lag_last_ns
                          << " lag_max_ns=" << m.lag_max_ns << '\n'
                          << std::flush;
                rate_at = now;
                rows_at = m.rows_total;
            }
        }
        const auto metrics = book.metrics();
        const auto life = book.life_state();
        std::cout << "rows_total=" << metrics.rows_total << " ignored_executions=" << metrics.ignored_executions
                  << " excluded_adds=" << metrics.excluded_adds << " lag_samples=" << metrics.lag_samples
                  << " lag_last_ns=" << metrics.lag_last_ns << " lag_max_ns=" << metrics.lag_max_ns
                  << " lag_sum_ns=" << metrics.lag_sum_ns << " crossed_transitions=" << metrics.crossed_transitions
                  << " native_life=" << life.last_lifenum << " info_trades_lifenum=" << life.info_trades_lifenum
                  << " metadata_refreshes=" << loop.metadata_refreshes << " polls=" << loop.polls << '\n';
        loop.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
