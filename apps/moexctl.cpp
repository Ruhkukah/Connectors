#include "moex/connector_host/operator_config.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "command_input.hpp"
#include "command_socket.hpp"
#include "scope_exit.hpp"

#include <array>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <span>
#include <stdexcept>
#include <unistd.h>

namespace {
#ifndef MOEX_SOURCE_GIT_SHA
#define MOEX_SOURCE_GIT_SHA "unknown"
#endif

std::string binary_sha256(const char* argv0) {
    try {
        std::error_code error;
        std::filesystem::path path{"/proc/self/exe"};
        if (std::filesystem::read_symlink(path, error).empty() || error) {
            path = std::filesystem::path(argv0 ? argv0 : "");
            // A bare argv[0] after PATH lookup is not an exact executable identity.
            if (path.empty() || (path.is_relative() && path.parent_path().empty()))
                return "unknown";
            path = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
        }
        // Open Linux's executable link itself, retaining the loaded inode even after deployment replaces its path.
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return "unknown";
        const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return moex::plaza2::cgate::plaza2_sha256_hex(std::as_bytes(std::span<const char>(bytes)));
    } catch (...) {
        return "unknown";
    }
}

volatile std::sig_atomic_t stopping{};
void stop(int signal) {
    stopping = signal;
}
struct ShutdownRequest {
    bool requested{}, force{};
    std::chrono::steady_clock::time_point deadline;
    std::string line, channel;
    void begin(bool forced, std::string_view input = {}, std::string_view source = {}) {
        if (!requested) {
            requested = true;
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            line = input;
            channel = source;
        }
        force |= forced;
    }
};
template <typename T> T integer(std::string_view text) {
    T out{};
    auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid numeric argument");
    return out;
}
std::string refusal(moex::connector_host::CgateTradingHost& host, std::string_view line, std::string_view error,
                    std::string_view channel) {
    host.record_local_refusal(line, error, channel);
    return "{\"ok\":false,\"error\":" + moex::connector_host::json_string(error) + "}";
}
std::string command(moex::connector_host::CgateTradingHost& host, ShutdownRequest& shutdown, std::string line,
                    std::string_view channel) {
    using namespace moex::connector_host;
    if (!line.empty())
        host.record_operator_input(line, channel);
    if (stopping && !shutdown.force) {
        host.set_kill_switch(true);
        shutdown.begin(true);
    }
    std::istringstream input(line);
    std::string verb, key, price, side, type, error;
    try {
        input >> verb;
        if (verb.empty())
            return refusal(host, line, "empty command", channel);
        if (verb == "quit") {
            std::string extra;
            input >> key;
            if ((!key.empty() && key != "--force") || (input >> extra))
                return refusal(host, line, "usage: quit [--force]", channel);
            const bool force = key == "--force";
            if (!force && !host.has_pending_cancellations() && host.has_working_orders())
                return refusal(host, line, "working orders remain; use quit --force", channel);
            if (!shutdown.requested)
                host.set_kill_switch(true);
            shutdown.begin(force, line, channel);
            return "{\"ok\":true,\"draining\":true,\"error\":\"\"}";
        }
        if (verb == "status")
            return host.status();
        if (verb == "place") {
            OrderRequest request;
            if (!(input >> request.client_order_id >> request.isin_id >> side >> request.quantity >> request.price))
                error = "usage: place ID ISIN buy|sell QUANTITY PRICE [day|ioc]";
            else if (side != "buy" && side != "sell")
                error = "side must be buy or sell";
            else {
                request.side = side == "buy" ? moex::plaza2_trade::Plaza2TradeSide::Buy
                                             : moex::plaza2_trade::Plaza2TradeSide::Sell;
                if (input >> type) {
                    if (type != "day" && type != "ioc")
                        error = "type must be day or ioc";
                    else
                        request.type = type == "ioc" ? moex::plaza2_trade::Plaza2TradeOrderType::Ioc
                                                     : moex::plaza2_trade::Plaza2TradeOrderType::Limit;
                }
                if (error.empty())
                    error = host.place(std::move(request));
            }
        } else if (verb == "cancel") {
            if (!(input >> key))
                error = "usage: cancel ID";
            else
                error = host.cancel(key);
        } else if (verb == "move") {
            std::int32_t quantity{};
            if (!(input >> key >> quantity >> price))
                error = "usage: move ID QUANTITY PRICE";
            else
                error = host.move(key, price, quantity);
        } else if (verb == "cancel-all") {
            std::int32_t isin{};
            if (!(input >> isin))
                error = "usage: cancel-all ISIN";
            else
                error = host.cancel_all(isin);
        } else if (verb == "kill") {
            if (!(input >> key) || (key != "on" && key != "off"))
                error = "usage: kill on|off";
            else if (shutdown.requested && key == "off")
                error = "shutdown is draining cancellations";
            else
                host.set_kill_switch(key == "on");
        } else if (verb == "storage") {
            std::string extra;
            if (!(input >> key) || key != "ok" || (input >> extra))
                error = "usage: storage ok";
            else
                error = host.storage_ok();
        } else
            error = "commands: place, cancel, move, cancel-all, kill, storage ok, status, quit";
    } catch (const std::invalid_argument& invalid) {
        error = invalid.what();
    }
    return error.empty() ? "{\"ok\":true,\"error\":\"\"}" : refusal(host, line, error, channel);
}
} // namespace
int main(int argc, char** argv) {
    using namespace moex::connector_host;
    std::signal(SIGHUP, SIG_IGN);
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        std::vector<std::string_view> arguments;
        std::filesystem::path log_path{"logs/moex_connector.ndjson"};
        std::filesystem::path state_path;
        std::filesystem::path socket_path;
        std::string login_env;
        std::optional<std::array<std::int32_t, 2>> ext_id_range;
        RiskLimits risk;
        bool overall_notional_configured{};
        std::uint32_t reply_timeout{60000};
        std::optional<std::int64_t> clock_offset;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--log" || arg == "--state" || arg == "--command-socket" || arg == "--max-quantity" ||
                arg == "--max-notional" || arg == "--max-position" || arg == "--max-open-orders" ||
                arg == "--reply-timeout-ms" || arg == "--clock-offset-us" || arg == "--login-env" ||
                arg == "--ext-id-range") {
                if (++i == argc)
                    throw std::invalid_argument("missing option value");
                const std::string_view value(argv[i]);
                if (arg == "--log")
                    log_path = value;
                else if (arg == "--state")
                    state_path = value;
                else if (arg == "--command-socket")
                    socket_path = value;
                else if (arg == "--login-env")
                    login_env = value;
                else if (arg == "--ext-id-range") {
                    const auto colon = value.find(':');
                    if (colon == std::string_view::npos)
                        throw std::invalid_argument("ext-id-range must be MIN:MAX");
                    const auto first = integer<std::int32_t>(value.substr(0, colon));
                    const auto last = integer<std::int32_t>(value.substr(colon + 1));
                    if (first <= 0 || last < first || last >= INT32_MAX)
                        throw std::invalid_argument("positive inclusive ext-id-range below INT32_MAX required");
                    ext_id_range = {{first, last}};
                } else if (arg == "--max-quantity") {
                    risk.max_quantity = integer<std::int32_t>(value);
                    risk.quantity_configured = true;
                } else if (arg == "--max-open-orders") {
                    risk.max_open_orders = integer<std::size_t>(value);
                    risk.open_orders_configured = true;
                } else if (arg == "--reply-timeout-ms")
                    reply_timeout = integer<std::uint32_t>(value);
                else if (arg == "--clock-offset-us")
                    clock_offset = integer<std::int64_t>(value);
                else {
                    const auto equal = value.find('=');
                    if (arg == "--max-position") {
                        if (equal == std::string_view::npos)
                            throw std::invalid_argument("max-position must be ISIN=CONTRACTS");
                        const auto isin = integer<std::int32_t>(value.substr(0, equal));
                        const auto cap = integer<std::int64_t>(value.substr(equal + 1));
                        if (isin <= 0 || cap <= 0 || !risk.max_position_by_isin.emplace(isin, cap).second)
                            throw std::invalid_argument("positive unique per-ISIN max-position required");
                        continue;
                    }
                    const auto decimal = moex::plaza2::private_state::parse_session_decimal(
                        equal == std::string_view::npos ? value : value.substr(equal + 1));
                    if (!decimal || decimal->units <= 0)
                        throw std::invalid_argument("positive max-notional required");
                    if (equal == std::string_view::npos) {
                        risk.max_notional_scaled = decimal->units;
                        overall_notional_configured = true;
                    } else {
                        const auto isin = integer<std::int32_t>(value.substr(0, equal));
                        if (isin <= 0 || !risk.max_notional_by_isin.emplace(isin, decimal->units).second)
                            throw std::invalid_argument("positive unique per-ISIN quote-notional limit required");
                    }
                }
            } else
                arguments.push_back(arg);
        }
        if (!overall_notional_configured && !risk.max_notional_by_isin.empty())
            risk.max_notional_scaled = INT64_MAX;
        if (socket_path.empty())
            socket_path = log_path.string() + ".sock";
        if (arguments.size() >= 2 && arguments[0] == "plaza2" && arguments[1] == "cmd") {
            if (arguments.size() != 3)
                throw std::invalid_argument("usage: moexctl plaza2 cmd [--log FILE | "
                                            "--command-socket PATH] \"COMMAND\"");
            const auto response = send_command(socket_path, std::string(arguments[2]));
            std::cout << response << '\n' << std::flush;
            return response.find("\"ok\":false") == std::string::npos ? 0 : 2;
        }
        const auto request = parse_operator_arguments(arguments);
        if (request.help) {
            std::cout << operator_help()
                      << "\nrun options: --log FILE --state FILE --max-quantity N "
                         "--max-notional ISIN=QUOTE_CAP "
                         "--max-position ISIN=CONTRACTS --max-open-orders N "
                         "--reply-timeout-ms N --clock-offset-us N --command-socket PATH\n"
                      << "required with --allow-orders: --login-env NAME --ext-id-range MIN:MAX "
                         "(deployment-assigned, nonoverlapping)\n"
                      << "run commands: place ID ISIN buy|sell QTY PRICE [day|ioc]; cancel "
                         "ID; move ID QTY PRICE; "
                         "cancel-all ISIN; kill on|off; storage ok; status; quit [--force]\n"
                      << "reconnect: moexctl plaza2 cmd [--log FILE | --command-socket "
                         "PATH] \"COMMAND\"\n"
                      << "command socket defaults to LOGFILE.sock; restricted to its "
                         "owner\n";
            return 0;
        }
        if (request.command == "run") {
            TradingHostConfig config;
            if (request.config.transport.host.allow_orders && (login_env.empty() || !ext_id_range))
                throw std::invalid_argument("--allow-orders requires --login-env and --ext-id-range");
            if (!login_env.empty()) {
                const auto* login = std::getenv(login_env.c_str());
                if (!login || !*login)
                    throw std::invalid_argument("required login environment variable is missing");
                config.orders.login_from = login;
            }
            if (ext_id_range) {
                config.orders.ext_id_begin = (*ext_id_range)[0];
                config.orders.ext_id_end = (*ext_id_range)[1];
                config.orders.ext_id_range_configured = true;
            }
            config.source_git_sha = MOEX_SOURCE_GIT_SHA;
            config.binary_sha256 = binary_sha256(argv[0]);
            config.session = request.config.transport.host;
            config.session.reply_timeout_ms = reply_timeout;
            config.orders.broker_code = request.config.order.broker_code;
            config.orders.client_code = request.config.order.client_code;
            config.orders.max_commands_per_second = config.session.publisher_messages_per_second;
            config.orders.reply_timeout = std::chrono::milliseconds(reply_timeout);
            config.orders.risk = risk;
            config.clock_offset_us = clock_offset;
            config.journal_path = log_path;
            config.identity_state_path = state_path;
            for (const auto isin : request.config.isin_ids) {
                if (isin <= 0 || isin > INT32_MAX)
                    throw std::invalid_argument("isin id outside CGate i4 range");
                config.isin_ids.push_back(static_cast<std::int32_t>(isin));
            }
            const auto signal_isins = config.isin_ids;
            const bool allow_orders = config.session.allow_orders;
            CgateTradingHost host(std::move(config));
            ScopeExit report([&] { host.report_outstanding_orders(std::cerr); });
            ScopeExit shutdown([&] { (void)host.stop(); });
            CommandSocket socket(socket_path);
            if (const auto error = host.start()) {
                std::cerr << error.message << '\n';
                return 3;
            }
            CommandInput input;
            ShutdownRequest drain;
            std::string signal_reason;
            const auto signal_shutdown = [&] {
                if (!stopping || !signal_reason.empty())
                    return;
                signal_reason = stopping == SIGINT ? "SIGINT" : "SIGTERM";
                host.record_operator_input(signal_reason, "signal");
                host.set_kill_switch(true);
                drain.begin(true);
                if (allow_orders)
                    for (const auto isin : signal_isins) {
                        const auto line = "cancel-all " + std::to_string(isin);
                        host.record_operator_input(line, "signal");
                        if (const auto error = host.cancel_all(isin); !error.empty())
                            host.record_local_refusal(line, error, "signal");
                    }
            };
            auto input_retry = std::chrono::steady_clock::now();
            for (;;) {
                signal_shutdown();
                if (const auto error = host.poll()) {
                    if (drain.requested)
                        host.record_shutdown_drain(signal_reason.empty() ? "quit" : signal_reason, false);
                    std::cerr << error.message << '\n';
                    return 3;
                }
                socket.poll([&](std::string line) { return command(host, drain, std::move(line), "command_socket"); },
                            [&](std::string_view line, std::string_view error) {
                                if (!line.empty())
                                    host.record_operator_input(line, "command_socket");
                                return refusal(host, line, error, "command_socket");
                            });
                pollfd descriptor{.fd = STDIN_FILENO, .events = POLLIN};
                if (std::chrono::steady_clock::now() >= input_retry && ::poll(&descriptor, 1, 0) > 0 &&
                    (descriptor.revents & (POLLIN | POLLHUP))) {
                    std::array<char, 4096> data{};
                    const auto count = ::read(STDIN_FILENO, data.data(), data.size());
                    if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                        input = CommandInput{};
                        input_retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
                    } else if (count > 0) {
                        input.feed(
                            std::string_view(data.data(), static_cast<std::size_t>(count)),
                            [&](const std::string& line) {
                                std::cout << command(host, drain, line, "stdin") << '\n' << std::flush;
                            },
                            [&](std::string_view error) {
                                std::cout << refusal(host, {}, error, "stdin") << '\n' << std::flush;
                            });
                    }
                }
                signal_shutdown();
                const bool waiting = host.has_pending_cancellations() ||
                                     (!signal_reason.empty() && allow_orders && host.has_working_orders());
                if (!drain.requested || (waiting && std::chrono::steady_clock::now() < drain.deadline))
                    continue;
                if (!drain.force && (host.has_pending_cancellations() || host.has_working_orders())) {
                    std::cout << refusal(host, drain.line,
                                         "cancellations incomplete or working orders remain; kill switch stays on; use "
                                         "quit --force",
                                         drain.channel)
                              << '\n'
                              << std::flush;
                    drain = {};
                    continue;
                }
                if (host.has_pending_cancellations())
                    std::cerr << "moexctl: cancellation drain reached its 10-second deadline; acknowledgements remain "
                                 "pending\n";
                host.record_shutdown_drain(signal_reason.empty() ? "quit" : signal_reason,
                                           waiting && std::chrono::steady_clock::now() >= drain.deadline);
                break;
            }
            const auto error = host.stop();
            shutdown.release();
            if (error) {
                std::cerr << error.message << '\n';
                return 7;
            }
            return 0;
        }
        ConnectorHost host(request.config);
        ScopeExit shutdown([&] { (void)host.stop(); });
        if (const auto error = host.start()) {
            std::cerr << error.message << '\n';
            return 3;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request.wait_ms);
        do {
            if (host.poll())
                break;
            if (host.snapshot().observation_ready)
                break;
        } while (std::chrono::steady_clock::now() < deadline);
        const auto snapshot = host.snapshot();
        std::cout << render_snapshot(snapshot, request.json);
        const auto error = host.stop();
        shutdown.release();
        if (error)
            return 7;
        return snapshot.observation_ready ? 0 : 4;
    } catch (const std::exception& error) {
        std::cerr << "moexctl: " << error.what() << '\n';
        return 2;
    }
}
