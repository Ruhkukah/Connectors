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
        auto config = moex::twime_trade::test::make_live_session_config(server.port(), "phase2f_live_runner");
        TwimeInMemorySessionPersistenceStore persistence;
        TwimeFakeClock clock(0);
        moex::twime_trade::test::ManualRunnerClock runner_clock;

        TwimeLiveSessionRunner runner(config, persistence, clock);
        runner.set_time_source([&runner_clock] { return runner_clock(); });

        std::exception_ptr server_error;
        std::thread server_thread([&] {
            try {
                moex::twime_sbe::test::require(server.wait_for_client(std::chrono::milliseconds(3000)),
                                               "expected live runner client connection");
                const auto establish = server.receive_up_to(1024, std::chrono::milliseconds(3000));
                const auto decoded = moex::twime_trade::test::decode_bytes(establish);
                moex::twime_sbe::test::require(decoded.metadata->name == "Establish", "runner must send Establish");

                auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
                for (auto& field : ack.fields) {
                    if (field.name == "NextSeqNo") {
                        field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(11);
                    }
                }
                server.send_bytes(moex::twime_trade::test::encode_bytes(ack));

                const auto terminate = server.receive_up_to(1024, std::chrono::milliseconds(3000));
                const auto terminate_decoded = moex::twime_trade::test::decode_bytes(terminate);
                moex::twime_sbe::test::require(terminate_decoded.metadata->name == "Terminate",
                                               "runner must send Terminate on stop");
                server.send_bytes(
                    moex::twime_trade::test::encode_bytes(moex::twime_trade::test::make_request("Terminate")));
                server.close_client();
            } catch (...) {
                server_error = std::current_exception();
            }
        });

        const auto start = runner.start();
        moex::twime_sbe::test::require(start.ok, "live session runner start must succeed for localhost");

        moex::twime_trade::test::pump_runner_until(runner, runner_clock, [](const TwimeLiveSessionRunner& candidate) {
            return candidate.health_snapshot().state == TwimeSessionState::Active;
        });

        const auto stop = runner.request_stop();
        moex::twime_sbe::test::require(stop.ok, "runner stop request must succeed");
        moex::twime_trade::test::pump_runner_until(runner, runner_clock, [](const TwimeLiveSessionRunner& candidate) {
            return candidate.health_snapshot().state == TwimeSessionState::Terminated ||
                   candidate.health_snapshot().state == TwimeSessionState::Faulted;
        });
        (void)runner.stop_if_needed();
        server_thread.join();
        if (server_error) {
            std::rethrow_exception(server_error);
        }

        moex::twime_sbe::test::require(runner.health_snapshot().state == TwimeSessionState::Terminated,
                                       "runner must terminate cleanly");
        moex::twime_sbe::test::require(!runner.ready_for_application_order_flow(),
                                       "Phase 2F must keep application order flow disabled by default");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
