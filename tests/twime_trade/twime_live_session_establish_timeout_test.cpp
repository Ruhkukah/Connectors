#include "moex/twime_trade/twime_live_session_runner.hpp"

#include "twime_live_session_test_support.hpp"

#include <exception>
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
        std::thread server_thread([&] {
            try {
                moex::twime_sbe::test::require(server.wait_for_client(std::chrono::milliseconds(3000)),
                                               "expected client connection");
                (void)server.receive_up_to(1024, std::chrono::milliseconds(3000));
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                server.close_client();
            } catch (...) {
                server_error = std::current_exception();
            }
        });

        moex::twime_sbe::test::require(runner.start().ok, "runner start must succeed");
        TwimeLiveSessionRunResult result{.ok = true};
        for (int i = 0; i < 16; ++i) {
            runner_clock.advance(25);
            result = runner.poll_once();
            if (!result.ok) {
                break;
            }
        }
        server_thread.join();
        if (server_error) {
            std::rethrow_exception(server_error);
        }

        moex::twime_sbe::test::require(!result.ok, "establish timeout must fault rather than silently hang");
        moex::twime_sbe::test::require(runner.health_snapshot().state == TwimeSessionState::Faulted,
                                       "establish timeout must surface Faulted state");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
