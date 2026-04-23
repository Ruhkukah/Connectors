#include "moex/plaza2_twime_reconciler/plaza2_twime_integrated_test_runner.hpp"

#include "plaza2_runtime_test_support.hpp"
#include "plaza2_twime_integrated_test_support.hpp"
#include "twime_live_session_test_support.hpp"

#include "local_tcp_server.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "expected fake runtime library path\n";
            return 1;
        }

        namespace plaza_test = moex::plaza2::test;
        namespace integrated_test = moex::plaza2_twime_reconciler::test;
        using namespace moex::plaza2_twime_reconciler;

        const auto fake_library = std::filesystem::path(argv[1]);
        const auto fixture_root = plaza_test::make_temp_directory("plaza2_twime_integrated_validation_test");
        const auto cleanup = [&]() { plaza_test::remove_tree(fixture_root); };

        const auto scheme_text = plaza_test::build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test");
        const auto fixture = plaza_test::materialize_runtime_fixture(
            fixture_root, fake_library, moex::plaza2::cgate::Plaza2Environment::Test, scheme_text);

        {
            moex::test::LocalTcpServer server;
            auto config =
                integrated_test::make_integrated_config(fixture, server.port(), "phase4c_missing_reconcile_arm");
            config.arm_state.test_reconcile_armed = false;

            moex::twime_trade::test::ScopedEnvVar twime_env("MOEX_TWIME_TEST_CREDENTIALS", "TWIME-SECRET");
            moex::twime_trade::test::ScopedEnvVar plaza_env("MOEX_PLAZA2_TEST_CREDENTIALS", "PLAZA-SECRET");

            Plaza2TwimeIntegratedTestRunner runner(std::move(config));
            const auto result = runner.start();
            integrated_test::require(!result.ok, "integrated runner must require --armed-test-reconcile");
            integrated_test::require(result.message.find("--armed-test-reconcile") != std::string::npos,
                                     "missing reconcile arm must be called out explicitly");
        }

        {
            moex::test::LocalTcpServer server;
            auto config =
                integrated_test::make_integrated_config(fixture, server.port(), "phase4c_missing_twime_creds");

            moex::twime_trade::test::ScopedEnvVar twime_env("MOEX_TWIME_TEST_CREDENTIALS", nullptr);
            moex::twime_trade::test::ScopedEnvVar plaza_env("MOEX_PLAZA2_TEST_CREDENTIALS", "PLAZA-SECRET");

            Plaza2TwimeIntegratedTestRunner runner(std::move(config));
            const auto result = runner.start();
            integrated_test::require(!result.ok, "integrated runner must fail when TWIME credentials are unavailable");
            integrated_test::require(result.message.find("TWIME TEST runner start failed") != std::string::npos,
                                     "TWIME-side startup failure must be surfaced");
        }

        {
            moex::test::LocalTcpServer server;
            auto config = integrated_test::make_integrated_config(fixture, server.port(), "phase4c_plaza_drift");
            config.plaza.runtime.expected_spectra_release = "SPECTRA95";

            moex::twime_trade::test::ScopedEnvVar twime_env("MOEX_TWIME_TEST_CREDENTIALS", "TWIME-SECRET");
            moex::twime_trade::test::ScopedEnvVar plaza_env("MOEX_PLAZA2_TEST_CREDENTIALS", "PLAZA-SECRET");

            Plaza2TwimeIntegratedTestRunner runner(std::move(config));
            const auto result = runner.start();
            integrated_test::require(!result.ok, "integrated runner must refuse incompatible PLAZA runtime drift");
            integrated_test::require(result.message.find("PLAZA II TEST runner start failed") != std::string::npos,
                                     "PLAZA-side startup failure must be surfaced");
        }

        cleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
