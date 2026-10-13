#include "harness.hpp"
#include "moex/connector_host/operator_config.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include "plaza2_trade_test_support.hpp"
#include <cstdlib>
#include <iostream>

using namespace moex::connector_host;
namespace cg = moex::plaza2::cgate;
namespace test = moex::plaza2::test;
namespace tr = moex::plaza2_trade;
int main(int argc, char** argv) {
    try {
        test::require(argc == 2, "fake library required");
        const auto root = test::make_temp_directory("shutdown-harness");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        test::fake::Control fake(fixture.library_path);
        ::setenv("MOEX_HARNESS_FAKE_CREDS", "fake-only", 1);
        ::setenv("MOEX_HARNESS_FAKE_KEY", "00000000", 1);
        Plaza2HostConfigInputs input;
        input.runtime_root = fixture.root;
        input.library_path = fixture.library_path;
        input.scheme_dir = fixture.scheme_dir;
        input.config_dir = fixture.config_dir;
        input.env_open_settings = "ini=config/t1.ini;key=00000000";
        input.credentials_env_var = "MOEX_HARNESS_FAKE_CREDS";
        input.software_key_env_var = "MOEX_HARNESS_FAKE_KEY";
        input.broker_code = "BRK1";
        input.client_code = "C01";
        input.isin_ids = {1001};
        input.allow_orders = true;
        TradingHostConfig config;
        config.session = build_plaza2_host_config(input).transport.host;
        config.session.mode = tr::CgateSessionMode::OfflineFake;
        config.session.process_timeout_ms = 0;
        config.session.trade_replay_from_pos_anchor = true;
        config.isin_ids = {1001};
        config.orders.broker_code = "BRK1";
        config.orders.client_code = "C01";
        config.orders.login_from = "test-owner";
        config.orders.sole_instance = true;
        config.orders.ext_id_begin = 100;
        config.orders.ext_id_end = 100000;
        config.orders.ext_id_range_configured = true;
        config.orders.command_rate_configured = true;
        config.orders.max_commands_per_second = 5;
        config.orders.risk.max_quantity = 2;
        config.orders.risk.quantity_configured = true;
        config.orders.risk.max_open_orders = 5;
        config.orders.risk.open_orders_configured = true;
        config.orders.risk.max_notional_by_isin[1001] = 30000000000LL;
        config.orders.risk.max_position_by_isin[1001] = 2;
        const auto configure = [&] {
            fake.configure({.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"});
            fake.set(test::fake::Option::PubReplyCode, "3");
        };
        int index{};
        const auto run = [&](TradingHostConfig candidate, ShutdownCase kind) {
            candidate.journal_path = root / (std::to_string(++index) + ".ndjson");
            candidate.identity_state_path = root / (std::to_string(index) + ".state");
            auto current = ShutdownReconnectHarness::Clock::time_point{};
            return ShutdownReconnectHarness::run(
                std::move(candidate), kind, [&] { return current; }, [&] { current += std::chrono::milliseconds(10); });
        };
        for (const auto kind : {ShutdownCase::Healthy, ShutdownCase::RecoveredEmpty, ShutdownCase::Unavailable,
                                ShutdownCase::SecondLoss}) {
            configure();
            const auto result = run(config, kind);
            test::require(result.failure.empty(), "case failure: " + result.failure);
            test::require(result.admitted && result.stop_ok && !result.pending_cancellations && !result.working_orders,
                          "case omitted admission/stop or queued cancellation");
            test::require(fake.commands().empty(), "no-post harness issued a native command");
            if (kind == ShutdownCase::Healthy || kind == ShutdownCase::RecoveredEmpty)
                test::require(result.shutdown_exit == 0 && !result.timed_out && !result.reconciliation_pending,
                              "healthy/recovered empty did not complete");
            else
                test::require(result.shutdown_exit == 8 && result.timed_out && result.reconciliation_pending,
                              "negative control falsely completed or lost ten-second bound");
            if (kind != ShutdownCase::Healthy)
                test::require(result.loss_observed && result.deferred_observed,
                              "controlled close did not exercise defer");
            if (kind == ShutdownCase::SecondLoss)
                test::require(result.recovered && result.second_loss_observed, "second loss proof not exercised");
        }
        for (const auto option : {test::fake::Option::DelayUserorderbook}) {
            configure();
            fake.set(option, "1");
            const auto result = run(config, ShutdownCase::RecoveredEmpty);
            test::require(!result.admitted && !result.loss_observed &&
                              result.failure == "fresh_flat_admission_unproven",
                          "incomplete native reconstruction admitted connection fault");
            test::require(fake.commands().empty(), "incomplete snapshot posted");
        }
        configure();
        fake.configure({.suppress_initial_orders = false, .zero_position = true, .client_code = "BRK1C01"});
        auto refused = run(config, ShutdownCase::RecoveredEmpty);
        test::require(!refused.admitted && refused.failure == "fresh_flat_admission_unproven",
                      "working orders admitted");
        configure();
        fake.configure({.suppress_initial_orders = true, .zero_position = false, .client_code = "BRK1C01"});
        refused = run(config, ShutdownCase::RecoveredEmpty);
        test::require(!refused.admitted && refused.failure == "fresh_flat_admission_unproven",
                      "nonflat account admitted");
        for (int invalid = 0; invalid < 8; ++invalid) {
            auto candidate = config;
            if (invalid == 0)
                candidate.session.runtime.environment = cg::Plaza2Environment::Prod;
            if (invalid == 1)
                candidate.orders.sole_instance = false;
            if (invalid == 2)
                candidate.session.trade_replay_from_pos_anchor = false;
            if (invalid == 3)
                candidate.session.private_streams.clear();
            if (invalid == 4)
                candidate.orders.risk.max_quantity = 3;
            if (invalid == 5)
                candidate.orders.max_commands_per_second = 6;
            if (invalid == 6)
                candidate.orders.risk.max_position_by_isin[1001] = 3;
            if (invalid == 7)
                candidate.orders.ext_id_end = candidate.orders.ext_id_begin + 6998;
            refused = run(candidate, ShutdownCase::Healthy);
            test::require(!refused.admitted && refused.failure == "invalid_isolated_test_profile",
                          "invalid profile admitted");
        }
        auto exhausted = config;
        exhausted.orders.next_ext_id = exhausted.orders.ext_id_end - 6998;
        refused = run(exhausted, ShutdownCase::Healthy);
        test::require(!refused.admitted && refused.failure == "remaining_id_capacity_unproven",
                      "remaining ID capacity gate bypassed");
        // Exercise the compile-time hard boundary with valid commands against
        // the same ACTIVE fake native publisher; no msgnew/post can occur.
        configure();
        tr::CgateSession session(config.session);
        test::require(!session.start(), "no-post session start");
        for (int i = 0; i < 30; ++i)
            test::require(!session.poll(false), "no-post bootstrap");
        auto add = tr::test_support::make_add_order();
        add.isin_id = 1001;
        auto move = tr::test_support::make_move_order();
        move.isin_id = 1001;
        const tr::Plaza2TradeCodec codec;
        const auto before = session.publisher_call_counts();
        std::uint32_t correlation = 1;
        for (const auto& command : {codec.encode(add), codec.encode(tr::test_support::make_del_order()),
                                    codec.encode(move), codec.encode(tr::test_support::make_del_user_orders())}) {
            test::require(command.validation.ok(), "valid negative-control command required");
            const auto posted = session.post_command(command, correlation++);
            test::require(posted.validation_error.code == cg::Plaza2ErrorCode::SendDisabledPreSendPhase &&
                              !posted.post_invoked &&
                              posted.certainty == cg::Plaza2SubmissionCertainty::DefinitelyNotSent,
                          "hard no-post boundary failed");
        }
        const auto after = session.publisher_call_counts();
        test::require(before.msgnew == after.msgnew && before.post == after.post && fake.commands().empty(),
                      "hard boundary reached native allocation/publication");
        test::require(!session.stop(), "no-post stop");
        std::cout << "shutdown harness offline cases PASS; no native session or certification evidence\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
