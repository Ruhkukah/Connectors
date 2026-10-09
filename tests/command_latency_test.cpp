#include "moex/connector_host/operator_config.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "command_socket.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include "private_delta_host_regression.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/resource.h>
#include <thread>

namespace {
using namespace moex::connector_host;
namespace test = moex::plaza2::test;
namespace cg = moex::plaza2::cgate;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
std::int64_t cpu_us() {
    rusage usage{};
    test::require(::getrusage(RUSAGE_SELF, &usage) == 0, "CPU accounting failed");
    return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000 + usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
}
double percentile(std::vector<std::int64_t> values, double rank) {
    test::require(!values.empty(), "latency samples missing");
    std::sort(values.begin(), values.end());
    if (rank == 0.5 && values.size() % 2 == 0)
        return (values[values.size() / 2 - 1] + values[values.size() / 2]) / 2000.0;
    return values[static_cast<std::size_t>(std::ceil(rank * values.size())) - 1] / 1000.0;
}
std::int64_t number(const std::string& line, std::string_view name) {
    const auto key = "\"" + std::string(name) + "\":";
    const auto offset = line.find(key);
    test::require(offset != std::string::npos, "command timing field missing: " + std::string(name));
    return std::stoll(line.substr(offset + key.size()));
}
void metric(std::string_view name, const std::vector<std::int64_t>& values) {
    std::cout << ",\"" << name << "\":{\"p50_us\":" << percentile(values, 0.5)
              << ",\"p99_us\":" << percentile(values, 0.99) << '}';
}

void run_case(TradingHostConfig config, const test::fake::Control& fake, const std::filesystem::path& root,
              bool tracing, bool measure_only) {
    constexpr std::size_t cycles = 100;
    const auto label = std::string(tracing ? "tracing-on" : "tracing-off");
    config.journal_path = root / (label + ".ndjson");
    config.identity_state_path = root / (label + ".state");
    config.measure_timings = false;
#ifdef MOEX_SPEED_BASELINE
    config.measure_timings = tracing;
#else
    config.measure_command_latency = tracing;
#endif
    fake.configure({.suppress_initial_orders = true,
                    .zero_position = true,
                    .simulate_idle_wait = true,
                    .client_code = "BRK1C01",
                    .session_id = 321});
    CommandSocket socket(label + ".sock");
    CgateTradingHost host(config);
    const auto initial_posts = fake.commands().size();
    test::require(!host.start(), "latency benchmark startup failed");
    for (int i = 0; i < 30; ++i)
        test::require(!host.poll(), "latency benchmark bootstrap failed");
    test::require(host.status().find("\"reconstructing\":false") != std::string::npos,
                  "latency benchmark never completed reconstruction");
    const auto native_wait = fake.last_process_timeout();
    if (!measure_only)
        test::require(native_wait > 0 && native_wait <= 2,
                      "idle native wait still exceeds 2 ms; command socket can wait behind it");

    std::vector<std::int64_t> receipt_to_place, place_to_post, receipt_to_post, place_total, socket_roundtrip;
    const auto refuse = [](std::string_view, std::string_view error) {
        return "{\"ok\":false,\"error\":" + json_string(error) + '}';
    };
    const auto execute = [&](std::string line, std::int64_t receipt) {
        std::istringstream input(line);
        std::string operation, key;
        input >> operation >> key;
        host.record_operator_input(line, "command_socket");
        std::string error;
        if (operation == "place") {
            fake.set(test::fake::Option::PubReplyOrderId, std::to_string(660000 + receipt_to_place.size()));
            OrderRequest request{.client_order_id = key, .isin_id = 1001, .price = "103000", .quantity = 1};
            const auto begin = steady_ns();
#ifdef MOEX_SPEED_BASELINE
            error = host.place(std::move(request));
#else
            error = host.place(std::move(request), receipt);
#endif
            const auto end = steady_ns(), posted = fake.last_post_started_steady_ns();
            if (error.empty()) {
                test::require(receipt > 0 && begin >= receipt && posted >= begin && end >= posted,
                              "fake publisher and socket clocks do not bracket the actual place/post path");
                receipt_to_place.push_back(begin - receipt);
                place_to_post.push_back(posted - begin);
                receipt_to_post.push_back(posted - receipt);
                place_total.push_back(end - begin);
            }
        } else if (operation == "cancel") {
            error = host.cancel(key);
            if (error.empty()) {
                const auto id = 660000 + std::stoll(key.substr(6));
                auto terminal = private_delta_host_detail::own_order(id, 1001, "BRK1C01");
                using enum moex::plaza2::generated::FieldCode;
                for (auto& field : terminal.fields) {
                    if (field.field_code == kFortsTradeReplOrdersLogPublicAction ||
                        field.field_code == kFortsTradeReplOrdersLogPrivateAction)
                        field.signed_value = 2;
                    if (field.field_code == kFortsTradeReplOrdersLogPublicAmountRest ||
                        field.field_code == kFortsTradeReplOrdersLogPrivateAmountRest)
                        field.signed_value = 0;
                }
                fake.enqueue({.kind = test::fake::EventKind::Begin,
                              .stream_code = moex::plaza2::generated::StreamCode::kFortsTradeRepl});
                fake.enqueue(terminal);
                fake.enqueue({.kind = test::fake::EventKind::Commit,
                              .stream_code = moex::plaza2::generated::StreamCode::kFortsTradeRepl});
            }
        } else
            error = "unexpected benchmark command";
        return error.empty() ? "{\"ok\":true}" : refuse(line, error);
    };

    // Account for all process threads, including the journal writer. There is
    // no client during this interval, so these are steady idle owner-loop costs.
    const auto idle_start = Clock::now();
    const auto idle_cpu = cpu_us();
    const auto idle_processes = fake.process_count();
    std::uint64_t idle_turns{};
    while (Clock::now() - idle_start < 5s) {
        test::require(!host.poll(), "latency benchmark idle poll failed");
        socket.poll(execute, refuse, true);
        ++idle_turns;
    }
    const auto idle_wall_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - idle_start).count();
    const auto idle_cpu_delta = cpu_us() - idle_cpu;
    const auto idle_native_calls = fake.process_count() - idle_processes;

    std::atomic<bool> finished{};
    std::exception_ptr client_error;
    const auto cpu_start = cpu_us(), wall_start = steady_ns();
    std::thread client([&] {
        try {
            for (std::size_t i = 0; i < cycles; ++i) {
                const auto key = "cycle-" + std::to_string(i);
                for (const auto& command : {"place " + key, "cancel " + key}) {
                    const auto begin = steady_ns();
                    const auto response = send_command(label + ".sock", command);
                    socket_roundtrip.push_back(steady_ns() - begin);
                    test::require(response.find("\"ok\":true") != std::string::npos,
                                  "benchmark product command refused: " + response);
                }
            }
        } catch (...) {
            client_error = std::current_exception();
        }
        finished.store(true);
    });
    ScopeExit join([&] { client.join(); });
    while (!finished.load()) {
        test::require(!host.poll(), "latency benchmark command poll failed");
        // The same receipt clock and fake-post hook are enabled in both cases;
        // only the product's command-timing instrumentation changes.
        socket.poll(execute, refuse, true);
    }
    client.join();
    join.release();
    if (client_error)
        std::rethrow_exception(client_error);
    const auto wall_ns = steady_ns() - wall_start, command_cpu_us = cpu_us() - cpu_start;
    for (int i = 0; i < 5; ++i)
        test::require(!host.poll(), "latency benchmark terminal reply drain failed");
    test::require(receipt_to_place.size() == cycles && fake.commands().size() == initial_posts + cycles * 2 &&
                      !host.has_working_orders(),
                  "benchmark did not complete 100 native Add/Del cycles without outstanding exposure");
    test::require(!host.stop(), "latency benchmark stop failed");

    std::ifstream journal(config.journal_path);
    std::string line;
    std::size_t timing_adds{}, timing_posts{}, slow_polls{};
    while (std::getline(journal, line)) {
        slow_polls += line.find("\"event\":\"slow_owner_poll\"") != std::string::npos;
        if (line.find("\"event\":\"command_timing\"") == std::string::npos)
            continue;
        ++timing_posts;
        if (line.find("\"command\":\"AddOrder\"") == std::string::npos)
            continue;
        ++timing_adds;
#ifndef MOEX_SPEED_BASELINE
        const auto a = number(line, "socket_receipt_to_place_ns"), b = number(line, "immediate_place_to_post_ns"),
                   total = number(line, "socket_receipt_to_post_ns");
        test::require(a >= 0 && b > 0 && total == a + b && number(line, "cg_pub_post_ns") >= 0,
                      "command-only timing lost a stage or mixed incompatible clocks");
#endif
    }
    test::require((tracing && timing_adds == cycles && timing_posts == cycles * 2) || (!tracing && timing_posts == 0),
                  "command tracing changed its coverage or instrumentation leaked into the disabled run");
    if (!measure_only)
        test::require(slow_polls == 0, "command-only timing still emitted per-poll instrumentation");
    std::cout << "SPD3 {\"case\":\"" << label << "\",\"cycles\":" << cycles << ",\"native_wait_ms\":" << native_wait
              << ",\"idle_wall_s\":" << idle_wall_us / 1000000.0
              << ",\"idle_cpu_core_percent\":" << idle_cpu_delta * 100.0 / idle_wall_us
              << ",\"idle_turns\":" << idle_turns << ",\"idle_native_calls\":" << idle_native_calls
              << ",\"command_wall_s\":" << wall_ns / 1000000000.0
              << ",\"command_cpu_core_percent\":" << command_cpu_us * 100000.0 / wall_ns
              << ",\"timing_records\":" << timing_posts << ",\"slow_poll_records\":" << slow_polls
              << ",\"journal_bytes\":" << std::filesystem::file_size(config.journal_path);
    metric("receipt_to_place", receipt_to_place);
    metric("place_to_post", place_to_post);
    metric("receipt_to_post", receipt_to_post);
    metric("place_total", place_total);
    metric("socket_roundtrip", socket_roundtrip);
    std::cout << "}\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        test::require(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--measure-only"),
                      "fake runtime path and optional --measure-only required");
        const auto root = test::make_temp_directory("cmd-latency");
        ScopeExit clean([&] { test::remove_tree(root); });
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        test::fake::Control fake(fixture.library_path);
        // Keep the socket inside the owned test directory without an absolute
        // pathname exceeding AF_UNIX limits in deep qualification trees.
        const auto original_directory = std::filesystem::current_path();
        std::filesystem::current_path(root);
        ScopeExit restore_directory([&] { std::filesystem::current_path(original_directory); });
        ::setenv("MOEX_SPEED_TEST_CREDENTIALS", "fake-test-only", 1);
        ::setenv("MOEX_SPEED_TEST_KEY", "00000000", 1);
        Plaza2HostConfigInputs input;
        input.runtime_root = fixture.root;
        input.library_path = fixture.library_path;
        input.scheme_dir = fixture.scheme_dir;
        input.config_dir = fixture.config_dir;
        input.env_open_settings = "ini=config/t1.ini;key=00000000";
        input.credentials_env_var = "MOEX_SPEED_TEST_CREDENTIALS";
        input.software_key_env_var = "MOEX_SPEED_TEST_KEY";
        input.broker_code = "BRK1";
        input.client_code = "C01";
        input.isin_ids = {1001};
        input.allow_orders = true;
        TradingHostConfig config;
        config.session = build_plaza2_host_config(input).transport.host;
        config.session.mode = moex::plaza2_trade::CgateSessionMode::OfflineFake;
        config.session.process_timeout_ms = 50; // Even legacy configurations must be capped on the live path.
        config.orders.broker_code = input.broker_code;
        config.orders.client_code = input.client_code;
        config.orders.login_from = "owner-login";
        config.orders.ext_id_range_configured = true;
        config.orders.sole_instance = true;
        config.orders.command_rate_configured = true;
        config.orders.max_commands_per_second = 3000;
        config.orders.risk.quantity_configured = config.orders.risk.open_orders_configured = true;
        config.orders.risk.max_quantity = 2;
        config.orders.risk.max_open_orders = 5;
        config.orders.risk.max_notional_by_isin[1001] = INT64_MAX;
        config.orders.risk.max_position_by_isin[1001] = 2;
        config.isin_ids = {1001};
        for (const bool tracing : {false, true})
            run_case(config, fake, root, tracing, argc == 3);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
