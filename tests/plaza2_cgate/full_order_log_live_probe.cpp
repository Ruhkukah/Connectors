// Explicitly invoked native read-only qualification tool; no publisher or order APIs.
#include "full_order_log_live_probe.hpp"
#include "moex/connector_host/operator_config.hpp"
#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>
#include <thread>
#include "moex/plaza2/cgate/cgate_logging.hpp"
namespace {
std::atomic_bool stopped{};
void stop(int) {
    stopped.store(true);
}
unsigned integer(std::string_view s) {
    unsigned value{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), value);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size())
        throw std::invalid_argument("invalid probe integer");
    return value;
}
void check(const moex::plaza2::cgate::Plaza2Error& e) {
    if (e)
        throw std::runtime_error(e.message);
}
} // namespace
int main(int argc, char** argv) {
    namespace cg = moex::plaza2::cgate;
    using namespace full_order_log_probe;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "full_order_log_live_probe --live --matching-id N --seconds 1800\n"
                         "  --trading-router HOST:PORT --trading-instance-id NAME\n"
                         "  plaza2 qualify [owner FullOrderLog runtime options; --router explicitly required]\n"
                         "No 4101 fallback. Matching must come from fresh committed REFDATA for all selected ISINs.\n"
                         "Creates composite and independent raw ORDBOOK listeners; no broker order APIs.\n"
                         "Bootstrap comparison requires identical native info.trades_lifenum and info.trades_rev.\n"
                         "Later log commits lack a proven global watermark and remain UNALIGNED.\n"
                         "Bounded SHA256 state history; missing alignment/P9 remains INCONCLUSIVE.\n";
            return 0;
        }
        bool live = false, matching_set = false;
        std::string router, trading_router, trading_instance;
        unsigned matching = 0, seconds = 1800;
        std::vector<std::string_view> args;
        for (int i = 1; i < argc; ++i) {
            const std::string_view key = argv[i];
            if (key == "--live")
                live = true;
            else if (key == "--matching-id" || key == "--seconds" || key == "--trading-router" ||
                     key == "--trading-instance-id") {
                require(++i < argc, "missing probe option value");
                if (key == "--matching-id") {
                    matching = integer(argv[i]);
                    matching_set = true;
                } else if (key == "--seconds")
                    seconds = integer(argv[i]);
                else if (key == "--trading-router")
                    trading_router = argv[i];
                else
                    trading_instance = argv[i];
            } else {
                if (key == "--router") {
                    require(i + 1 < argc, "missing FullOrderLog router endpoint");
                    router = argv[i + 1];
                }
                args.push_back(key);
            }
        }
        require(live && matching_set && !router.empty() && !trading_router.empty() && !trading_instance.empty() &&
                    matching <= 127 && seconds >= 1 && seconds <= 3600,
                "explicit live/matching/FullLog and trading endpoints/instance and bounded duration are required");
        const auto port = [](std::string_view endpoint) {
            const auto colon = endpoint.rfind(':');
            require(colon != std::string_view::npos && colon > 0 && colon + 1 < endpoint.size(),
                    "router must be host:port");
            const auto value = integer(endpoint.substr(colon + 1));
            require(value > 0 && value <= 65535, "invalid router port");
            return value;
        };
        require(port(router) != 4101 && port(router) != port(trading_router),
                "FullLog must use a separate router port");
        if (std::ranges::find(args, "--instance-id") == args.end())
            args.insert(args.end(), {"--instance-id", "moex_full_order_log_probe"});
        args.push_back("--read-only-market-data");
        auto request = moex::connector_host::parse_operator_arguments(args);
        require(request.command == "qualify", "read-only qualify command required");
        auto config = request.config.transport.host;
        const auto app_start = config.connection_settings.find(";app_name=") + 10;
        const auto app =
            config.connection_settings.substr(app_start, config.connection_settings.find(';', app_start) - app_start);
        require(app != trading_instance, "probe and trading instance IDs must differ");
        const auto key = cg::load_plaza2_credentials(config.software_key);
        require(key && !key->value.empty(), "configured CGate software key source is missing");
        const auto credentials = cg::load_plaza2_credentials(config.credentials);
        const auto replace = [](std::string& text, const std::string& token, const std::string& value) {
            for (auto at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size()))
                text.replace(at, token.size(), value);
        };
        replace(config.runtime.env_open_settings, "${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}", key->value);
        if (!config.software_key.env_var.empty())
            replace(config.runtime.env_open_settings, "${" + config.software_key.env_var + "}", key->value);
        if (credentials) {
            for (auto* text : {&config.runtime.env_open_settings, &config.connection_settings}) {
                replace(*text, "${MOEX_PLAZA2_CREDENTIALS}", credentials->value);
                replace(*text, "${PLAZA2_CREDENTIALS}", credentials->value);
                if (!config.credentials.env_var.empty())
                    replace(*text, "${" + config.credentials.env_var + "}", credentials->value);
            }
        }
        cg::validate_cgate_logging(config.runtime.env_open_settings, config.runtime.config_dir);
        require(config.runtime.environment == cg::Plaza2Environment::Test, "qualification probe is TEST-only");
        std::vector<std::int32_t> ids(request.config.isin_ids.begin(), request.config.isin_ids.end());
        require(ids.size() <= 64, "bounded probe universe exceeded");
        std::sort(ids.begin(), ids.end());
        cg::Plaza2FullOrderLog book(ids);
        Window window(4096, &std::cout);
        Observer composite(window, ids, &book, 200000, &std::cout), reference(window, ids, nullptr, 200000, &std::cout);
        cg::Plaza2Env env;
        check(env.open(config.runtime));
        cg::Plaza2Connection connection;
        check(connection.create(env, config.connection_settings));
        check(connection.open(""));
        const auto ready_until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        std::uint32_t state{};
        while (true) {
            check(connection.state(state));
            if (state == 3)
                break;
            require(std::chrono::steady_clock::now() < ready_until, "FullOrderLog connection did not become active");
            check(connection.process(10));
        }
        const auto suffix = "_MATCH" + std::to_string(matching);
        cg::Plaza2Listener full, raw;
        check(full.create(connection, cg::kFullOrderLogStreamCode,
                          "p2ordbook://FORTS_ORDLOG_REPL" + suffix + ";snapshot=FORTS_ORDBOOK_REPL" + suffix,
                          &composite));
        check(raw.create(connection, cg::kFullOrderLogStreamCode, "p2repl://FORTS_ORDBOOK_REPL" + suffix, &reference));
        check(full.open(""));
        check(raw.open("mode=snapshot+online"));
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        const auto bootstrap_until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!book.valid() || !window.raw_snapshots) {
            require(std::chrono::steady_clock::now() < bootstrap_until, "native comparison bootstrap did not complete");
            check(connection.process(1));
            check(full.last_callback_error());
            check(raw.last_callback_error());
        }
        const auto start = std::chrono::steady_clock::now();
        auto next = start;
        auto previous = start;
        auto previous_rows = book.metrics().rows_total;
        std::cout << "{\"kind\":\"probe_started\",\"matching_id\":" << matching << ",\"requested_seconds\":" << seconds
                  << ",\"publisher_handles\":0,\"order_posts\":0}\n"
                  << std::flush;
        while (!stopped.load() && std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
            check(connection.process(1));
            check(full.last_callback_error());
            check(raw.last_callback_error());
            check(full.state(state));
            require(state != 1, "composite listener failed; probe interrupted");
            check(raw.state(state));
            require(state != 1, "reference listener failed; probe interrupted");
            const auto now = std::chrono::steady_clock::now();
            if (now >= next) {
                const auto m = book.metrics();
                const auto elapsed = std::chrono::duration<double>(now - previous).count();
                std::cout << "{\"kind\":\"metrics\",\"rows_total\":" << m.rows_total
                          << ",\"rows_per_second\":" << (elapsed > 0 ? (m.rows_total - previous_rows) / elapsed : 0)
                          << ",\"lag_samples\":" << m.lag_samples << ",\"lag_last_ns\":" << m.lag_last_ns
                          << ",\"lag_max_ns\":" << m.lag_max_ns << ",\"valid\":" << (book.valid() ? "true" : "false")
                          << ",\"matched\":" << window.matched << ",\"mismatched\":" << window.mismatched
                          << ",\"p9_events\":" << composite.p9_events << ",\"p9_erased\":" << composite.p9_erased
                          << "}\n"
                          << std::flush;
                previous = now;
                previous_rows = m.rows_total;
                next = now + std::chrono::seconds(30);
            }
        }
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        check(full.state(state));
        const bool composite_active = state == 3;
        check(raw.state(state));
        const bool reference_active = state == 3;
        check(connection.state(state));
        const bool connection_active = state == 3;
        const bool current = book.valid() && composite_active && reference_active && connection_active;
        const auto unresolved_raw = std::count_if(window.states[1].begin(), window.states[1].end(),
                                                  [](const auto& item) { return !item.second.paired; });
        std::cout
            << "{\"kind\":\"probe_result\",\"elapsed_seconds\":" << elapsed << ",\"matched\":" << window.matched
            << ",\"mismatched\":" << window.mismatched << ",\"ambiguous\":" << window.ambiguous
            << ",\"history_evictions\":" << window.evicted << ",\"raw_snapshots\":" << window.raw_snapshots
            << ",\"composite_commits\":" << window.composite_commits << ",\"p9_events\":" << composite.p9_events
            << ",\"p9_erased\":" << composite.p9_erased << ",\"unresolved_raw_publications\":" << unresolved_raw
            << ",\"unanchored_log_commits\":" << composite.unanchored_commits
            << ",\"unresolved_raw_retired\":" << window.unresolved_raw_retired
            << ",\"composite_active\":" << (composite_active ? "true" : "false")
            << ",\"reference_active\":" << (reference_active ? "true" : "false")
            << ",\"connection_active\":" << (connection_active ? "true" : "false")
            << ",\"duration_complete\":" << (elapsed >= seconds ? "true" : "false")
            << ",\"current\":" << (current ? "true" : "false")
            << ",\"comparison_scope\":\"bootstrap_snapshot_anchors_only\",\"p9_qualified\":false,\"complete\":false"
            << "}\n";
        check(raw.close());
        check(full.close());
        check(connection.close());
        return window.mismatched ? 4 : 3; // Exact live log-state alignment is unsupported, never claim qualification.
    } catch (const std::exception& e) {
        std::cerr << cg::redact_plaza2_setting_value(e.what()) << '\n';
        return 2;
    }
}
