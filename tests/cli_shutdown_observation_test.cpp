#define main moexctl_embedded_signal_main
#include "../apps/moexctl.cpp"
#undef main
#include "fake_cgate_control.hpp"
#include "plaza2_runtime_test_support.hpp"

namespace test = moex::plaza2::test;
namespace cg = moex::plaza2::cgate;
using namespace moex::connector_host;
int main(int argc, char** argv) {
    try {
        test::require(argc == 2, "fake runtime required");
        ShutdownSignals signals; // Before the host's journal worker or CGate startup.
        const auto root = test::make_temp_directory("cli-shutdown-observe");
        ScopeExit clean([&] { test::remove_tree(root); });
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        test::fake::Control fake(fixture.library_path);
        fake.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
        ::setenv("SIGNAL_TEST_CREDENTIALS", "fake-only", 1);
        ::setenv("SIGNAL_TEST_KEY", "00000000", 1);
        Plaza2HostConfigInputs input;
        input.runtime_root = fixture.root;
        input.library_path = fixture.library_path;
        input.scheme_dir = fixture.scheme_dir;
        input.config_dir = fixture.config_dir;
        input.env_open_settings = "ini=config/t1.ini;key=00000000";
        input.credentials_env_var = "SIGNAL_TEST_CREDENTIALS";
        input.software_key_env_var = "SIGNAL_TEST_KEY";
        input.broker_code = "BRK1";
        input.client_code = "C01";
        input.isin_ids = {1001};
        input.allow_orders = true;
        TradingHostConfig config;
        config.session = build_plaza2_host_config(input).transport.host;
        config.session.mode = moex::plaza2_trade::CgateSessionMode::OfflineFake;
        config.orders.broker_code = input.broker_code;
        config.orders.client_code = input.client_code;
        config.orders.login_from = "fake-owner";
        config.orders.ext_id_range_configured = config.orders.sole_instance = config.orders.command_rate_configured =
            true;
        config.orders.max_commands_per_second = 5;
        config.orders.risk.quantity_configured = config.orders.risk.open_orders_configured = true;
        config.orders.risk.max_quantity = 2;
        config.orders.risk.max_open_orders = 5;
        config.orders.risk.max_notional_by_isin[1001] = INT64_MAX;
        config.orders.risk.max_position_by_isin[1001] = 2;
        config.isin_ids = {1001};
        config.journal_path = root / "signal.ndjson";
        config.identity_state_path = root / "identity.state";
        CgateTradingHost host(config);
        ScopeExit stop([&] { (void)host.stop(); });
        test::require(!host.start(), "host start failed");
        for (int turn = 0; turn < 20; ++turn)
            test::require(!host.poll(), "bootstrap poll failed");
        test::require(
            host.place({.client_order_id = "signal-working", .isin_id = 1001, .price = "103000", .quantity = 1})
                .empty(),
            "working Add was refused");
        for (int turn = 0; turn < 5; ++turn)
            test::require(!host.poll(), "working Add poll failed");
        test::require(host.has_working_orders(), "working order precondition missing");
        const auto processes = fake.process_count();
        const auto posts = fake.commands().size();
        test::require(::kill(::getpid(), SIGTERM) == 0, "real SIGTERM delivery failed");
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!signals.requested() && std::chrono::steady_clock::now() < end)
            std::this_thread::yield();
        ShutdownRequest drain;
        std::string reason;
        // This is the exact function called by the real CLI owner loop, with
        // no host poll permitted between signal observation and cancellation.
        observe_shutdown_signal(signals, host, drain, reason, config.isin_ids, true);
        const auto commands = fake.commands();
        test::require(reason == "SIGTERM" && drain.requested && drain.force && commands.size() == posts + 1 &&
                          commands.back().name == "DelUserOrders" && fake.process_count() == processes,
                      "signal cancellation was deferred to a later owner poll");
        observe_shutdown_signal(signals, host, drain, reason, config.isin_ids, true);
        test::require(fake.commands().size() == commands.size(), "first signal cancellation was repeated");
        std::cout << "Actual CLI signal observation posts DelUserOrders in the same owner iteration PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
