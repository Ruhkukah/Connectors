#include "moex/connector_host/dtc_market_data.hpp"
#include "moex/connector_host/dtc_read_only_server.hpp"
#include "moex/connector_host/operator_config.hpp"
#include "moex/plaza2/cgate/plaza2_runtime.hpp"
#include "moex/plaza2/cgate/plaza2_text.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <csignal>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using moex::connector_host::ConnectorHost;
using moex::connector_host::ConnectorHostMarketDataSnapshot;
using moex::connector_host::OperatorRequest;
using moex::connector_host::dtc::ConnectorHostDtcMarketDataSource;
using moex::connector_host::dtc::DtcMarketDataSnapshot;
using moex::connector_host::dtc::DtcReadOnlyServer;
using moex::connector_host::dtc::DtcReadOnlyServerConfig;
using moex::connector_host::dtc::DtcSourceMode;
namespace cg = moex::plaza2::cgate;

#ifndef MOEX_SOURCE_GIT_SHA
#define MOEX_SOURCE_GIT_SHA "unknown"
#endif

constexpr std::uint16_t kDefaultDtcPort = 11200;
constexpr std::uint32_t kDefaultSymbolId = 1;
constexpr std::uint32_t kDefaultStartupWaitMs = 10000;
constexpr std::string_view kDefaultDtcUsernameEnv = "MOEX_PLAZA_DTC_USERNAME";
constexpr std::string_view kDefaultDtcPasswordEnv = "MOEX_PLAZA_DTC_PASSWORD";

std::atomic_bool stop_requested{false};

void signal_handler(int) noexcept {
    stop_requested.store(true, std::memory_order_relaxed);
}

template <class T> T positive_integer(std::string_view value, std::string_view option, T maximum) {
    T out{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || out == 0 || out > maximum)
        throw std::invalid_argument(std::string(option) + " must be a positive bounded integer");
    return out;
}

std::string required_environment(std::string_view variable) {
    const std::string name(variable);
    const char* value = std::getenv(name.c_str());
    if (!value || !*value)
        throw std::invalid_argument("required DTC credential environment variable is missing");
    return value;
}

struct Options {
    std::vector<std::string_view> host_arguments;
    std::string board;
    std::string currency;
    std::uint16_t port{kDefaultDtcPort};
    std::uint32_t symbol_id{kDefaultSymbolId};
    std::uint32_t startup_wait_ms{kDefaultStartupWaitMs};
    bool require_auth{false};
    std::string username_env{kDefaultDtcUsernameEnv};
    std::string password_env{kDefaultDtcPasswordEnv};
};

constexpr std::string_view runner_help = R"(moex_connector_host_dtc_runner plaza2 qualify [ConnectorHost options]
TEST-only, strict read-only ConnectorHost market-data runner.
Optional operator bindings: --dtc-board BOARD --dtc-currency CODE
DTC:       --dtc-port N (default 11200, loopback only)
           --dtc-symbol-id N (default 1)
           --startup-wait-ms N (default 10000, maximum 60000)
Auth:      --require-dtc-auth
           --dtc-username-env NAME --dtc-password-env NAME
           (defaults: MOEX_PLAZA_DTC_USERNAME / MOEX_PLAZA_DTC_PASSWORD)
The DTC credentials are dedicated local-gateway credentials. They are never
read from or copied into the CGate/T1 credential settings. Omit auth options
to run without the optional local DTC logon gate.

The runner polls ConnectorHost, samples its target book, then polls DTC on one
owner thread. It binds only 127.0.0.1, creates no publisher or p2mqreply
surface, exposes no orders/accounts, and never authorizes or submits orders.
Missing authoritative live metadata leaves DTC 506 unavailable (509) and
revokes DTC source authority on the next owner poll. No economics or clock
semantics are inferred. See operator_help() for ConnectorHost TEST options.
)";

Options parse_options(int argc, char** argv) {
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        const auto value = [&](std::string_view option) {
            if (++i >= argc)
                throw std::invalid_argument(std::string(option) + " requires a value");
            return std::string_view(argv[i]);
        };
        if (arg == "--dtc-board") {
            if (!out.board.empty())
                throw std::invalid_argument("duplicate --dtc-board");
            out.board = value(arg);
            if (out.board.empty())
                throw std::invalid_argument("--dtc-board must not be empty");
        } else if (arg == "--dtc-currency") {
            if (!out.currency.empty())
                throw std::invalid_argument("duplicate --dtc-currency");
            out.currency = value(arg);
            if (out.currency.empty())
                throw std::invalid_argument("--dtc-currency must not be empty");
        } else if (arg == "--dtc-port") {
            out.port = positive_integer<std::uint16_t>(value(arg), arg, std::numeric_limits<std::uint16_t>::max());
        } else if (arg == "--dtc-symbol-id") {
            out.symbol_id = positive_integer<std::uint32_t>(value(arg), arg, std::numeric_limits<std::uint32_t>::max());
        } else if (arg == "--startup-wait-ms") {
            out.startup_wait_ms = positive_integer<std::uint32_t>(value(arg), arg, 60000U);
        } else if (arg == "--require-dtc-auth") {
            if (out.require_auth)
                throw std::invalid_argument("duplicate --require-dtc-auth");
            out.require_auth = true;
        } else if (arg == "--dtc-username-env") {
            if (out.username_env != kDefaultDtcUsernameEnv)
                throw std::invalid_argument("duplicate --dtc-username-env");
            out.username_env = value(arg);
        } else if (arg == "--dtc-password-env") {
            if (out.password_env != kDefaultDtcPasswordEnv)
                throw std::invalid_argument("duplicate --dtc-password-env");
            out.password_env = value(arg);
        } else {
            out.host_arguments.push_back(arg);
        }
    }
    if ((!out.board.empty() && (!cg::text::valid_utf8(out.board) || out.board.size() > 128)) ||
        (!out.currency.empty() && (!cg::text::valid_utf8(out.currency) || out.currency.size() > 16)))
        throw std::invalid_argument("DTC board/currency metadata is invalid UTF-8 or exceeds its bound");
    if (!out.require_auth && (out.username_env != kDefaultDtcUsernameEnv || out.password_env != kDefaultDtcPasswordEnv))
        throw std::invalid_argument("dedicated DTC credential environment overrides require --require-dtc-auth");
    const auto dedicated_dtc_environment_name = [](std::string_view name) {
        constexpr std::string_view prefix = "MOEX_PLAZA_DTC_";
        if (!name.starts_with(prefix) || name.size() == prefix.size())
            return false;
        for (const auto ch : name.substr(prefix.size()))
            if (!(ch == '_' || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')))
                return false;
        return true;
    };
    if (!dedicated_dtc_environment_name(out.username_env) || !dedicated_dtc_environment_name(out.password_env))
        throw std::invalid_argument("DTC credential environment names must use the MOEX_PLAZA_DTC_* namespace");
    return out;
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const auto ch : value) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
                out += "?";
            else
                out.push_back(ch);
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::string binary_sha256(const char* argv0) {
    try {
        std::error_code error;
        // On Linux this is the exact loaded executable even when argv[0] was
        // resolved through PATH or replaced by a launcher.
        auto path = std::filesystem::read_symlink("/proc/self/exe", error);
        if (error || path.empty()) {
            path = std::filesystem::path(argv0 ? argv0 : "");
            // A bare argv[0] is only a process name after PATH lookup; it is
            // not an exact binary identity. Linux normally takes the
            // /proc/self/exe path above, otherwise fail closed.
            if (path.empty() || (path.is_relative() && path.parent_path().empty()))
                return "unknown";
        }
        if (path.empty())
            return "unknown";
        if (path.is_relative())
            path = std::filesystem::current_path() / path;
        path = std::filesystem::weakly_canonical(path);
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return "unknown";
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return cg::plaza2_sha256_hex(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
    } catch (...) {
        return "unknown";
    }
}

bool positive_decimal(std::string_view text) {
    if (text.empty() || text.size() > 64)
        return false;
    try {
        std::size_t end = 0;
        const auto value = std::stof(std::string(text), &end);
        return end == text.size() && std::isfinite(value) && value > 0;
    } catch (...) {
        return false;
    }
}

bool metadata_507_ready(const DtcMarketDataSnapshot& snapshot) {
    return snapshot.refdata_metadata_current && snapshot.refdata_vcb_join_current &&
           !snapshot.refdata_vcb_join_ambiguous && snapshot.future_vcb_provenance_present &&
           snapshot.refdata_board_proven && snapshot.refdata_currency_proven && snapshot.isin_id > 0 &&
           !snapshot.symbol.empty() && !snapshot.board.empty() && cg::text::valid_utf8(snapshot.symbol) &&
           cg::text::valid_utf8(snapshot.board) && !snapshot.description.empty() &&
           snapshot.description.size() <= 512 && cg::text::valid_utf8(snapshot.description) &&
           !snapshot.currency.empty() && snapshot.currency.size() <= 16 && cg::text::valid_utf8(snapshot.currency) &&
           positive_decimal(snapshot.min_step) && positive_decimal(snapshot.contract_size) &&
           positive_decimal(snapshot.currency_value_per_increment);
}

bool authority_ready(const DtcMarketDataSnapshot& snapshot) {
    // A same-session LateJoinCorroboratedSnapshot can permit provisional
    // display while target_authoritative remains false. It is still strictly
    // non-executable and must be reported as provisional, not as warmup.
    return snapshot.valid && snapshot.transport_active && snapshot.aggr_online && snapshot.snapshot_complete &&
           snapshot.book_snapshot_current && snapshot.market_data_display_allowed &&
           (snapshot.target_authoritative ||
            snapshot.session_ready_witness_kind == cg::SessionReadyWitnessKind::LateJoinCorroboratedSnapshot);
}

void print_startup_receipt(const Options& options, const DtcReadOnlyServer& server, const DtcMarketDataSnapshot& source,
                           std::string_view binary_identity, bool publisher_handle_open, bool reply_handle_open) {
    const auto board_provenance = source.refdata_board_proven ? "refdata_fut_vcb"
                                  : options.board.empty()     ? "missing"
                                                              : "operator_binding_not_refdata_proof";
    const auto currency_provenance = source.refdata_vcb_join_current && source.future_vcb_provenance_present
                                         ? source.refdata_currency_proven ? "refdata_fut_vcb_rub_supported"
                                                                          : "refdata_fut_vcb_unsupported_denomination"
                                     : options.currency.empty() ? "missing"
                                                                : "operator_binding_not_refdata_proof";
    const auto vcb_join = source.refdata_vcb_join_current     ? "resolved"
                          : source.refdata_vcb_join_ambiguous ? "ambiguous"
                                                              : "missing";
    std::cout << "{\"event\":\"connector_host_dtc_runner_startup\""
              << ",\"source_mode\":\"live_test\",\"target_environment\":\"TEST\""
              << ",\"source_git_sha\":" << json_escape(MOEX_SOURCE_GIT_SHA)
              << ",\"binary_sha256\":" << json_escape(binary_identity) << ",\"dtc_bind\":\"127.0.0.1\""
              << ",\"dtc_port\":" << server.port() << ",\"dtc_symbol_id\":" << server.symbol_id()
              << ",\"configured_board\":" << json_escape(options.board)
              << ",\"configured_currency\":" << json_escape(options.currency) << ",\"session_id\":" << source.session_id
              << ",\"board\":" << json_escape(source.board) << ",\"currency\":" << json_escape(source.currency)
              << ",\"board_provenance\":" << json_escape(board_provenance)
              << ",\"currency_provenance\":" << json_escape(currency_provenance)
              << ",\"refdata_vcb_join\":" << json_escape(vcb_join)
              << ",\"refdata_fut_vcb_source\":\"FORTS_REFDATA_REPL.fut_vcb\""
              << ",\"refdata_fut_vcb_provenance_present\":" << (source.future_vcb_provenance_present ? "true" : "false")
              << ",\"future_vcb_repl_rev\":" << source.future_vcb_repl_rev
              << ",\"future_vcb_lifenum\":" << source.future_vcb_lifenum << ",\"symbol\":" << json_escape(source.symbol)
              << ",\"isin_id\":" << source.isin_id << ",\"min_step\":" << json_escape(source.min_step)
              << ",\"read_only\":true,\"order_entry_allowed\":false,\"accounts\":false"
              << ",\"positions\":false,\"orders\":false,\"publisher_handle_open\":"
              << (publisher_handle_open ? "true" : "false")
              << ",\"reply_handle_open\":" << (reply_handle_open ? "true" : "false")
              << ",\"no_publisher_surface\":" << (!publisher_handle_open && !reply_handle_open ? "true" : "false")
              << ",\"authority_ready\":" << (authority_ready(source) ? "true" : "false")
              << ",\"market_data_display_allowed\":" << (source.market_data_display_allowed ? "true" : "false")
              << ",\"target_authoritative\":" << (source.target_authoritative ? "true" : "false")
              << ",\"authority_witness\":"
              << json_escape(cg::session_ready_witness_kind_name(source.session_ready_witness_kind))
              << ",\"metadata_507_ready\":" << (metadata_507_ready(source) ? "true" : "false") << "}" << '\n'
              << std::flush;
}

void print_help() {
    std::cout << runner_help << '\n' << moex::connector_host::operator_help();
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            print_help();
            return 0;
        }
        const auto options = parse_options(argc, argv);
        auto host_arguments = options.host_arguments;
        host_arguments.push_back("--read-only-market-data");
        const auto request = moex::connector_host::parse_operator_arguments(host_arguments);
        if (request.help) {
            print_help();
            return 0;
        }
        if (request.command != "qualify" || request.config.purpose != moex::connector_host::HostPurpose::Qualify)
            throw std::invalid_argument("runner requires `plaza2 qualify`; order-test/status are not runner modes");

        // Resolve the exact loaded executable before creating the native
        // ConnectorHost or DTC listener. An unidentified binary never gets a
        // warmup window or a listening socket.
        const auto identity = binary_sha256(argc > 0 ? argv[0] : nullptr);
        if (identity == "unknown") {
            std::cerr << "running binary identity is unavailable; refusing to start an unidentified binary\n";
            return 9;
        }
        std::string local_username;
        std::string local_password;
        if (options.require_auth) {
            // Resolve and bound-check operator-provided local DTC credentials
            // before native CGate startup. Values are never logged.
            local_username = required_environment(options.username_env);
            local_password = required_environment(options.password_env);
        }

        auto host_config = request.config;
        host_config.target_board = options.board;
        host_config.target_currency = options.currency;
        host_config.read_only_market_data = true;
        ConnectorHost host(std::move(host_config));
        ConnectorHostDtcMarketDataSource source(host);
        DtcReadOnlyServerConfig server_config;
        server_config.port = options.port;
        server_config.source_mode = DtcSourceMode::LiveTest;
        server_config.symbol_id = options.symbol_id;
        server_config.require_local_auth = options.require_auth;
        server_config.local_username = std::move(local_username);
        server_config.local_password = std::move(local_password);
        // Constructing the bounded server validates all local auth and queue
        // limits before either host or listener startup below.
        DtcReadOnlyServer server(source, std::move(server_config));
        if (const auto error = host.start()) {
            std::cerr << "ConnectorHost TEST start failed: " << error.message << '\n';
            return 3;
        }
        bool host_started = true;
        const auto no_publisher_surface = [&] {
            const auto snapshot = host.snapshot();
            return !snapshot.publisher_handle_open && !snapshot.reply_handle_open;
        };
        if (!no_publisher_surface()) {
            std::cerr << "readonly runner refused a publisher/reply surface\n";
            static_cast<void>(host.stop());
            return 4;
        }
        std::string listen_error;
        if (!server.start(listen_error)) {
            std::cerr << "DTC loopback start failed: " << listen_error << '\n';
            static_cast<void>(host.stop());
            return 5;
        }

        DtcMarketDataSnapshot sampled = source.snapshot();
        const auto warmup_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(options.startup_wait_ms);
        int result = 0;
        while (!stop_requested.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < warmup_deadline) {
            // The ownership order is intentional and fixed: ConnectorHost
            // poll, one target snapshot, then DTC poll.
            const auto host_error = host.poll();
            sampled = source.snapshot();
            server.poll();
            if (!no_publisher_surface()) {
                std::cerr << "readonly runner observed a publisher/reply surface and revoked service\n";
                result = 6;
                break;
            }
            if (host_error) {
                std::cerr << "ConnectorHost TEST poll failed: " << host_error.message << '\n';
                result = 7;
                break;
            }
            if (authority_ready(sampled) && metadata_507_ready(sampled))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto startup_host_snapshot = host.snapshot();
        print_startup_receipt(options, server, sampled, identity, startup_host_snapshot.publisher_handle_open,
                              startup_host_snapshot.reply_handle_open);

        while (result == 0 && !stop_requested.load(std::memory_order_relaxed)) {
            // Keep this loop in the same pollhost -> sample -> DTC order.
            const auto host_error = host.poll();
            sampled = source.snapshot();
            server.poll();
            if (!no_publisher_surface()) {
                std::cerr << "readonly runner observed a publisher/reply surface and revoked service\n";
                result = 6;
                break;
            }
            if (host_error) {
                std::cerr << "ConnectorHost TEST poll failed: " << host_error.message << '\n';
                result = 7;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        server.stop();
        if (host_started) {
            if (const auto error = host.stop()) {
                std::cerr << "ConnectorHost TEST stop failed: " << error.message << '\n';
                result = result == 0 ? 8 : result;
            }
        }
        return result;
    } catch (const std::invalid_argument& error) {
        std::cerr << "moex_connector_host_dtc_runner: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "moex_connector_host_dtc_runner: operation failed\n";
        return 3;
    }
}
