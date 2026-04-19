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
        auto config = moex::twime_trade::test::make_live_session_config(server.port(), "phase2f_live_reject");
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
                const auto establish = server.receive_up_to(1024, std::chrono::milliseconds(3000));
                const auto decoded = moex::twime_trade::test::decode_bytes(establish);
                moex::twime_sbe::test::require(decoded.metadata->name == "Establish", "runner must send Establish");
                auto reject = moex::twime_trade::test::make_request("EstablishmentReject");
                server.send_bytes(moex::twime_trade::test::encode_bytes(reject));
            } catch (...) {
                server_error = std::current_exception();
            }
        });

        moex::twime_sbe::test::require(runner.start().ok, "runner start must succeed");
        moex::twime_trade::test::pump_runner_until(runner, runner_clock, [](const TwimeLiveSessionRunner& candidate) {
            return candidate.health_snapshot().state == TwimeSessionState::Rejected;
        });
        server.close_client();
        server_thread.join();
        if (server_error) {
            std::rethrow_exception(server_error);
        }

        moex::twime_sbe::test::require(runner.health_snapshot().reject_seen,
                                       "health snapshot must expose establishment reject");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
