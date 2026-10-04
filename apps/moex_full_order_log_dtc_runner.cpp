#include "full_order_log_dtc_loop.hpp"
#include "moex/connector_host/operator_config.hpp"
#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>
#include <map>
#include <algorithm>
#include <cctype>

namespace {
namespace dtc = moex::connector_host::dtc;
namespace cg = moex::plaza2::cgate;
std::atomic_bool stop_requested{};
void stop_signal(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}
template <class T = std::uint32_t> T integer(std::string_view value) {
    T result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !result)
        throw std::invalid_argument("expected positive integer");
    return result;
}
std::string router_endpoint(std::string_view address) {
    auto [endpoint, port] = moex::connector_host::router_address(address);
    std::ranges::transform(endpoint, endpoint.begin(), [](unsigned char c) { return std::tolower(c); });
    if (endpoint == "localhost" || endpoint == "localhost." || endpoint == "[::1]" || endpoint == "::1" ||
        endpoint.starts_with("127."))
        endpoint = "loopback";
    return endpoint + ':' + std::to_string(port);
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "moex_full_order_log_dtc_runner plaza2 qualify [read-only runtime options]\n"
                         "  --dtc-port N (default 11300, consecutive loopback ports for configured ISINs)\n"
                         "  --live --trading-instance-id NAME (MOEX/broker entitlement is the owner responsibility)\n"
                         "  --router HOST:PORT (default 127.0.0.1:4102; separate order-log router/login)\n"
                         "  --trading-router HOST:PORT (default 127.0.0.1:4101; must differ from --router)\n"
                         "  --offline-fake --run-ms N (offline tests; no live authorization inferred)\n"
                         "  --dtc-depth N (default 20; up to 20000 per side; NumLevels=0 uses that cap)\n"
                         "Order entry, accounts, positions and market-by-order are unavailable.\n"
                      << moex::connector_host::operator_help();
            return 0;
        }
        bool offline = false, live = false;
        std::string trading_instance, trading_router = "127.0.0.1:4101";
        std::uint32_t base_port = 11300, run_ms = 0, depth_levels = 20;
        const std::map<std::string_view, std::uint32_t*> numeric_options{
            {"--dtc-port", &base_port}, {"--dtc-depth", &depth_levels}, {"--run-ms", &run_ms}};
        std::vector<std::string_view> arguments;
        for (int i = 1; i < argc; ++i) {
            const std::string_view key = argv[i];
            if (key == "--offline-fake")
                offline = true;
            else if (key == "--live")
                live = true;
            else if (key == "--trading-instance-id" || key == "--trading-router" || numeric_options.contains(key)) {
                if (++i == argc)
                    throw std::invalid_argument("missing runner option value");
                if (key == "--trading-instance-id")
                    trading_instance = argv[i];
                else if (key == "--trading-router")
                    trading_router = argv[i];
                else
                    *numeric_options.at(key) = integer(argv[i]);
            } else
                arguments.push_back(key);
        }
        if (offline == live)
            throw std::invalid_argument("select --live or --offline-fake");
        if (live && trading_instance.empty())
            throw std::invalid_argument("--live requires the trading process --trading-instance-id");
        for (const auto& [key, value] :
             {std::pair{"--router", "127.0.0.1:4102"}, std::pair{"--instance-id", "moex_full_order_log"}})
            if (std::ranges::find(arguments, key) == arguments.end())
                arguments.insert(arguments.end(), {key, value});
        arguments.push_back("--read-only-market-data");
        auto request = moex::connector_host::parse_operator_arguments(arguments);
        const auto& connection = request.config.transport.host.connection_settings;
        if (router_endpoint(connection.substr(8, connection.find(';') - 8)) == router_endpoint(trading_router))
            throw std::invalid_argument("order-log and trading router addresses must differ");
        const auto app_name = connection.substr(connection.find(";app_name=") + 10);
        if (app_name.substr(0, app_name.find(';')) == (trading_instance.empty() ? "moex_connector" : trading_instance))
            throw std::invalid_argument("order-log and trading instance IDs must differ");
        if (request.command != "qualify")
            throw std::invalid_argument("FullOrderLog runner requires plaza2 qualify");
        if (request.config.isin_ids.size() > 64 || depth_levels > 20000 || base_port > UINT16_MAX ||
            request.config.isin_ids.size() - 1 > UINT16_MAX - base_port)
            throw std::invalid_argument("invalid bounded ISIN universe or DTC port range");
        std::vector<std::int32_t> ids(request.config.isin_ids.begin(), request.config.isin_ids.end());
        cg::Plaza2FullOrderLog book(ids);
        auto& transport = request.config.transport.host;
        transport.mode =
            offline ? moex::plaza2_trade::CgateSessionMode::OfflineFake : moex::plaza2_trade::CgateSessionMode::Live;
        transport.aggr20_stream = {};
        transport.public_deals_stream = {};
        transport.process_timeout_ms = 1;
        transport.full_order_log_stream = {.stream_code = cg::kFullOrderLogStreamCode,
                                           .settings = "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL"};
        transport.full_order_log_handler = &book;
        transport.event_log = [](std::string_view event, std::string_view detail) {
            std::cerr << event << ' ' << detail << '\n';
        };
        transport.listener_event_log = [](const cg::Plaza2ListenerEvent& event) {
            if (event.stream_code == cg::kFullOrderLogStreamCode)
                std::cerr << "FullOrderLog listener event " << static_cast<unsigned>(event.kind)
                          << " life=" << event.unsigned_value << " revision=" << event.signed_value
                          << " table=" << (event.raw_table ? event.raw_table->name : "unspecified") << '\n';
        };
        moex::connector_host::ConnectorHost host(std::move(request.config));
        moex::connector_host::FullOrderLogDtcLoop loop(
            host, book,
            {.port = static_cast<std::uint16_t>(base_port),
             .source_mode = offline ? dtc::DtcSourceMode::Replay : dtc::DtcSourceMode::LiveTest,
             .max_queued_bytes = 4 * 1024 * 1024,
             .max_depth_levels = depth_levels});
        book.on_crossed = [](std::int32_t isin, bool crossed) {
            std::cerr << "FullOrderLog crossed isin=" << isin << " crossed=" << crossed << '\n';
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
        const auto report = [&] {
            const auto now = std::chrono::steady_clock::now();
            const auto metrics = book.metrics();
            const auto life = book.committed_log_lifenum();
            std::cout << "rows_per_second="
                      << (metrics.rows_total - rows_at) / std::chrono::duration<double>(now - rate_at).count()
                      << " info_life_available=" << life.has_value() << " info_trades_lifenum=" << life.value_or(0)
                      << " rows_total=" << metrics.rows_total << " ignored_executions=" << metrics.ignored_executions
                      << " excluded_adds=" << metrics.excluded_adds << " lag_samples=" << metrics.lag_samples
                      << " lag_last_ns=" << metrics.lag_last_ns << " lag_max_ns=" << metrics.lag_max_ns
                      << " clear_deleted_events=" << metrics.clear_deleted_events
                      << " clear_deleted_erased=" << metrics.clear_deleted_erased
                      << " lag_sum_ns=" << metrics.lag_sum_ns << " crossed_transitions=" << metrics.crossed_transitions
                      << " metadata_refreshes=" << loop.metadata_refreshes << " polls=" << loop.polls << '\n'
                      << std::flush;
            rate_at = now;
            rows_at = metrics.rows_total;
        };
        while (!stop_requested.load(std::memory_order_relaxed) &&
               (!run_ms || std::chrono::steady_clock::now() - start < std::chrono::milliseconds(run_ms))) {
            loop.poll();
            if (std::chrono::steady_clock::now() - rate_at >= std::chrono::seconds(30))
                report();
        }
        report();
        loop.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
