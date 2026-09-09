#include "moex/twime_trade/twime_live_session_runner.hpp"

#include "twime_live_session_test_support.hpp"

#include <exception>
#include <atomic>
#include <iostream>
#include <thread>

int main() {
    try {
        using namespace moex::twime_trade;

        moex::test::LocalTcpServer server;
        moex::twime_trade::test::ScopedEnvVar env("MOEX_TWIME_TEST_CREDENTIALS", "LOGIN");
        auto config = moex::twime_trade::test::make_live_session_config(server.port(), "phase2f_live_timeout");
        config.policy.establish_deadline_ms = 100;
        TwimeInMemorySessionPersistenceStore persistence;
        TwimeFakeClock clock(0);
        moex::twime_trade::test::ManualRunnerClock runner_clock;

        TwimeLiveSessionRunner runner(config, persistence, clock);
        runner.set_time_source([&runner_clock] { return runner_clock(); });

        std::exception_ptr server_error;
        std::atomic<bool> establish_received{false};
        std::atomic<bool> timeout_checked{false};
        std::jthread server_thread([&] {
            try {
                moex::twime_sbe::test::require(server.wait_for_client(std::chrono::milliseconds(3000)),
                                               "expected client connection");
                moex::twime_sbe::test::require(!server.receive_up_to(1024, std::chrono::milliseconds(3000)).empty(),
                                               "expected Establish bytes");
                establish_received = true;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (!timeout_checked && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                server.close_client();
            } catch (...) {
                server_error = std::current_exception();
            }
        });

        moex::twime_sbe::test::require(runner.start().ok, "runner start must succeed");
        // A fixed number of polls can expire before the OS finishes connecting.
        // Hold fake time still until Establish is actually sent; keep the peer
        // open so a remote close cannot masquerade as the timeout being tested.
        const auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!establish_received && std::chrono::steady_clock::now() < connect_deadline) {
            moex::twime_sbe::test::require(runner.poll_once().ok, "poll until Establish is received");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        moex::twime_sbe::test::require(establish_received, "Establish must reach the peer before advancing time");
        moex::twime_sbe::test::require(runner.poll_once().ok, "drain Establish event with frozen clock");
        runner_clock.advance(100);
        const auto boundary = runner.poll_once();
        runner_clock.advance(1);
        const auto result = runner.poll_once();
        timeout_checked = true;
        server_thread.join();
        if (server_error) {
            std::rethrow_exception(server_error);
        }

        moex::twime_sbe::test::require(boundary.ok, "deadline is not exceeded at the exact boundary");
        moex::twime_sbe::test::require(result.message == "Establish deadline exceeded",
                                       "fault must be the establish deadline");
        moex::twime_sbe::test::require(!result.ok, "establish timeout must fault rather than silently hang");
        moex::twime_sbe::test::require(runner.health_snapshot().state == TwimeSessionState::Faulted,
                                       "establish timeout must surface Faulted state");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
