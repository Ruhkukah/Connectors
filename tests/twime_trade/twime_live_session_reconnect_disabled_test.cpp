#include "moex/twime_trade/twime_live_session_runner.hpp"

#include "twime_live_session_test_support.hpp"

#include <atomic>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>

int main() {
    try {
        using namespace moex::twime_trade;

        moex::test::LocalTcpServer server;
        moex::twime_trade::test::ScopedEnvVar env("MOEX_TWIME_TEST_CREDENTIALS", "LOGIN");
        auto config = moex::twime_trade::test::make_live_session_config(server.port(), "phase2f_live_reconnect_off");
        config.policy.reconnect_enabled = false;
        TwimeInMemorySessionPersistenceStore persistence;
        TwimeFakeClock clock(0);
        moex::twime_trade::test::ManualRunnerClock runner_clock;
        std::atomic<bool> active_seen{false};

        TwimeLiveSessionRunner runner(config, persistence, clock);
        runner.set_time_source([&runner_clock] { return runner_clock(); });

        std::exception_ptr server_error;
        std::thread server_thread([&] {
            try {
                moex::twime_sbe::test::require(server.wait_for_client(std::chrono::milliseconds(3000)),
                                               "expected client connection");
                (void)server.receive_up_to(1024, std::chrono::milliseconds(3000));
                auto ack = moex::twime_trade::test::make_request("EstablishmentAck");
                for (auto& field : ack.fields) {
                    if (field.name == "NextSeqNo") {
                        field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(11);
                    }
                }
                server.send_bytes(moex::twime_trade::test::encode_bytes(ack));
                for (int attempt = 0; attempt < 200 && !active_seen.load(); ++attempt) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                server.close_client();
            } catch (...) {
                server_error = std::current_exception();
            }
        });
        const auto join_server = [&] {
            if (server_thread.joinable()) {
                server_thread.join();
            }
            if (server_error) {
                std::rethrow_exception(server_error);
            }
        };
        struct JoinGuard {
            decltype(join_server)& fn;
            ~JoinGuard() {
                fn();
            }
        } join_guard{join_server};

        moex::twime_sbe::test::require(runner.start().ok, "runner start must succeed");
        moex::twime_trade::test::pump_runner_until(runner, runner_clock, [](const TwimeLiveSessionRunner& candidate) {
            return candidate.health_snapshot().state == TwimeSessionState::Active;
        });
        active_seen.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int i = 0; i < 16; ++i) {
            runner_clock.advance(25);
            (void)runner.poll_once();
        }
        moex::twime_sbe::test::require(runner.health_snapshot().state == TwimeSessionState::Faulted,
                                       "remote close with reconnect disabled must leave session faulted");
        moex::twime_sbe::test::require(!runner.reconnect_due(),
                                       "reconnect must remain disabled when reconnect_enabled=false");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
