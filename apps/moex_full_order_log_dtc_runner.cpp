#include "moex/connector_host/full_order_log_dtc.hpp"
#include "moex/connector_host/dtc_read_only_server.hpp"
#include "moex/connector_host/operator_config.hpp"
#include <atomic>
#include <charconv>
#include <csignal>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

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
                         "  --entitlement-confirmation FILE --owner-approved-live\n"
                         "  --offline-fake --run-ms N (offline tests; no live authorization inferred)\n"
                         "  --dtc-depth N (default 20; up to 20000 per side; NumLevels=0 uses that cap)\n"
                         "Each port serves one configured ISIN from the full anonymous book.\n"
                         "Order entry, accounts, positions and market-by-order are unavailable.\n"
                      << moex::connector_host::operator_help();
            return 0;
        }
        bool offline = false, approved = false, instance = false;
        std::string entitlement;
        std::uint32_t base_port = 11300, run_ms = 0, depth_levels = 20;
        std::vector<std::string_view> arguments;
        for (int i = 1; i < argc; ++i) {
            const std::string_view key = argv[i];
            if (key == "--offline-fake")
                offline = true;
            else if (key == "--owner-approved-live")
                approved = true;
            else if (key == "--entitlement-confirmation" || key == "--dtc-port" || key == "--run-ms" ||
                     key == "--dtc-depth") {
                if (++i == argc)
                    throw std::invalid_argument("missing runner option value");
                if (key == "--entitlement-confirmation")
                    entitlement = argv[i];
                else if (key == "--dtc-port")
                    base_port = integer(argv[i]);
                else if (key == "--dtc-depth")
                    depth_levels = integer(argv[i]);
                else
                    run_ms = integer(argv[i]);
            } else {
                instance |= key == "--instance-id";
                arguments.push_back(key);
            }
        }
        if (!offline) {
            std::ifstream confirmation(entitlement);
            if (!approved || !confirmation || confirmation.peek() == std::ifstream::traits_type::eof())
                throw std::invalid_argument("live FullOrderLog requires written MOEX/broker entitlement confirmation "
                                            "FILE and --owner-approved-live");
        }
        arguments.push_back("--read-only-market-data");
        if (!instance) {
            arguments.push_back("--instance-id");
            arguments.push_back("moex_full_order_log");
        }
        auto request = moex::connector_host::parse_operator_arguments(arguments);
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
                          << " table=" << event.message_name << '\n';
        };
        moex::connector_host::ConnectorHost host(std::move(request.config));
        std::vector<std::unique_ptr<dtc::DtcFullOrderLogSource>> sources;
        std::vector<std::unique_ptr<dtc::DtcReadOnlyServer>> servers;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            sources.push_back(std::make_unique<dtc::DtcFullOrderLogSource>(book, ids[i], dtc::DtcMarketDataSnapshot{},
                                                                           depth_levels * 2));
            dtc::DtcReadOnlyServerConfig config;
            config.port = static_cast<std::uint16_t>(base_port + i);
            config.symbol_id = static_cast<std::uint32_t>(i + 1);
            config.source_mode = offline ? dtc::DtcSourceMode::Replay : dtc::DtcSourceMode::LiveTest;
            config.max_depth_levels = depth_levels;
            config.max_queued_bytes = 4 * 1024 * 1024;
            servers.push_back(std::make_unique<dtc::DtcReadOnlyServer>(*sources.back(), config));
        }
        struct CallbackLifetime {
            cg::Plaza2FullOrderLog& book;
            ~CallbackLifetime() {
                book.on_commit = {};
                book.on_invalidate = {};
            }
        } callback_lifetime{book};
        book.on_commit = [&](const auto&) {
            for (std::size_t i = 0; i < sources.size(); ++i) {
                sources[i]->committed();
                servers[i]->publish_depth_commit();
            }
        };
        book.on_invalidate = [&] {
            for (auto& server : servers)
                server->publish_depth_commit();
        };
        std::signal(SIGINT, stop_signal);
        std::signal(SIGTERM, stop_signal);
        if (const auto error = host.start(); error)
            throw std::runtime_error(error.message);
        for (std::size_t i = 0; i < servers.size(); ++i) {
            std::string error;
            if (!servers[i]->start(error))
                throw std::runtime_error(error);
            std::cout << "isin=" << ids[i] << " dtc_port=" << servers[i]->port() << " symbol_id=" << i + 1
                      << " source_mode=" << (offline ? "offline_fake" : "live_test") << '\n';
        }
        std::cout << "book_memory_bytes=" << book.memory_bytes()
                  << " max_queue_bytes_per_isin=4194304 dtc_depth_per_side=" << depth_levels << '\n'
                  << std::flush;
        const auto start = std::chrono::steady_clock::now();
        while (!stop_requested.load(std::memory_order_relaxed) &&
               (!run_ms || std::chrono::steady_clock::now() - start < std::chrono::milliseconds(run_ms))) {
            if (const auto error = host.poll(); error)
                throw std::runtime_error(error.message);
            for (std::size_t i = 0; i < sources.size(); ++i) {
                sources[i]->update_metadata(dtc::make_dtc_market_data_snapshot(host.market_data_snapshot(ids[i])));
                servers[i]->poll();
            }
        }
        for (auto& server : servers)
            server->stop();
        if (const auto error = host.stop(); error)
            throw std::runtime_error(error.message);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
