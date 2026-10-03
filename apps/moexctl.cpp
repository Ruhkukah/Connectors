#include "moex/connector_host/operator_config.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "command_input.hpp"
#include "command_socket.hpp"
#include "scope_exit.hpp"

#include <array>
#include <charconv>
#include <csignal>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping{};
void stop(int) {
    stopping = 1;
}
template <typename T> T integer(std::string_view text) {
    T out{};
    auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid numeric argument");
    return out;
}
std::string command(moex::connector_host::CgateTradingHost& host, std::string line) {
    using namespace moex::connector_host;
    std::istringstream input(line);
    std::string verb, key, price, side, type, error;
    input >> verb;
    if (verb.empty())
        return "{\"ok\":false,\"error\":\"empty command\"}";
    if (verb == "quit") {
        stopping = 1;
        return "{\"ok\":true,\"error\":\"\"}";
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
            request.side =
                side == "buy" ? moex::plaza2_trade::Plaza2TradeSide::Buy : moex::plaza2_trade::Plaza2TradeSide::Sell;
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
        else
            host.set_kill_switch(key == "on");
    } else
        error = "commands: place, cancel, move, cancel-all, kill, status, quit";
    return std::string("{\"ok\":") + (error.empty() ? "true" : "false") + ",\"error\":" + json_string(error) + "}";
}
} // namespace
int main(int argc, char** argv) {
    using namespace moex::connector_host;
    std::signal(SIGHUP, SIG_IGN);
    std::signal(SIGPIPE, SIG_IGN);
    try {
        std::vector<std::string_view> arguments;
        std::filesystem::path log_path{"logs/moex_connector.ndjson"};
        std::filesystem::path state_path;
        std::filesystem::path socket_path;
        RiskLimits risk;
        std::uint32_t reply_timeout{60000};
        std::optional<std::int64_t> clock_offset;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--log" || arg == "--state" || arg == "--command-socket" || arg == "--max-quantity" ||
                arg == "--max-notional" || arg == "--max-open-orders" || arg == "--reply-timeout-ms" ||
                arg == "--clock-offset-us") {
                if (++i == argc)
                    throw std::invalid_argument("missing option value");
                const std::string_view value(argv[i]);
                if (arg == "--log")
                    log_path = value;
                else if (arg == "--state")
                    state_path = value;
                else if (arg == "--command-socket")
                    socket_path = value;
                else if (arg == "--max-quantity")
                    risk.max_quantity = integer<std::int32_t>(value);
                else if (arg == "--max-open-orders")
                    risk.max_open_orders = integer<std::size_t>(value);
                else if (arg == "--reply-timeout-ms")
                    reply_timeout = integer<std::uint32_t>(value);
                else if (arg == "--clock-offset-us")
                    clock_offset = integer<std::int64_t>(value);
                else {
                    const auto decimal = moex::plaza2::private_state::parse_session_decimal(value);
                    if (!decimal || decimal->units <= 0)
                        throw std::invalid_argument("positive max-notional required");
                    risk.max_notional_scaled = decimal->units;
                }
            } else
                arguments.push_back(arg);
        }
        if (socket_path.empty())
            socket_path = log_path.string() + ".sock";
        if (arguments.size() >= 2 && arguments[0] == "plaza2" && arguments[1] == "cmd") {
            if (arguments.size() != 3)
                throw std::invalid_argument(
                    "usage: moexctl plaza2 cmd [--log FILE | --command-socket PATH] \"COMMAND\"");
            std::cout << send_command(socket_path, std::string(arguments[2])) << '\n' << std::flush;
            return 0;
        }
        const auto request = parse_operator_arguments(arguments);
        if (request.help) {
            std::cout << operator_help()
                      << "\nrun options: --log FILE --state FILE --max-quantity N --max-notional N --max-open-orders N "
                         "--reply-timeout-ms N --clock-offset-us N --command-socket PATH\n"
                      << "run commands: place ID ISIN buy|sell QTY PRICE [day|ioc]; cancel ID; move ID QTY PRICE; "
                         "cancel-all ISIN; kill on|off; status; quit\n"
                      << "reconnect: moexctl plaza2 cmd [--log FILE | --command-socket PATH] \"COMMAND\"\n"
                      << "command socket defaults to LOGFILE.sock; restricted to its owner\n";
            return 0;
        }
        if (request.command == "run") {
            TradingHostConfig config;
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
            CgateTradingHost host(std::move(config));
            ScopeExit report([&] { host.report_outstanding_orders(std::cerr); });
            ScopeExit shutdown([&] { (void)host.stop(); });
            if (const auto error = host.start()) {
                std::cerr << error.message << '\n';
                return 3;
            }
            std::signal(SIGINT, stop);
            std::signal(SIGTERM, stop);
            CommandSocket socket(socket_path);
            CommandInput input;
            auto input_retry = std::chrono::steady_clock::now();
            while (!stopping) {
                if (const auto error = host.poll()) {
                    std::cerr << error.message << '\n';
                    return 3;
                }
                socket.poll([&](std::string line) { return command(host, std::move(line)); });
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
                            [&](const std::string& line) { std::cout << command(host, line) << '\n'
                                                                     << std::flush; },
                            [](std::string_view error) {
                                std::cout << "{\"ok\":false,\"error\":" << json_string(error) << "}\n" << std::flush;
                            });
                    }
                }
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
