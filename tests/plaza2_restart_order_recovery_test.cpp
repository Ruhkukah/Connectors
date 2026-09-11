#include "moex/connector_host/operator_config.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "plaza2_trade/fixtures/cgate99_messages.hpp"

#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <iostream>

namespace {
using namespace moex::connector_host;
using namespace moex::plaza2_trade;
namespace cg = moex::plaza2::cgate;
namespace test = moex::plaza2::test;
template <class T>
concept RawPublisher = requires(T& host) { host.publisher(); };
template <class T>
concept RawTransport = requires(T& host) { host.transport(); };
template <class T>
concept RawPost = requires(T& host) { host.post(); };
static_assert(!RawPublisher<ConnectorHost> && !RawTransport<ConnectorHost> && !RawPost<ConnectorHost>);

Plaza2HostConfig config_for(const test::RuntimeFixturePaths& f) {
    const std::vector<std::string> owned{"plaza2",
                                         "qualify",
                                         "--runtime-root",
                                         f.root.string(),
                                         "--scheme-dir",
                                         f.scheme_dir.string(),
                                         "--config-dir",
                                         f.config_dir.string(),
                                         "--env-settings-var",
                                         "HOST_TEST_ENV",
                                         "--broker-code-env",
                                         "HOST_TEST_BROKER",
                                         "--client-code-env",
                                         "HOST_TEST_CLIENT",
                                         "--isin-id",
                                         "1001",
                                         "--session-id",
                                         "321",
                                         "--expected-release",
                                         "SPECTRA93",
                                         "--armed-test-network",
                                         "--armed-test-session",
                                         "--armed-test-plaza2"};
    std::vector<std::string_view> args(owned.begin(), owned.end());
    auto config = parse_operator_arguments(args).config;
    config.transport.host.process_timeout_ms = 0;
    auto& c = config.order;
    c.profile_id = "offline-plaza2-test";
    c.profile_fingerprint = std::string(64, 'e');
    c.base_contract_code = "RTS";
    c.instrument_mask = 1;
    c.side = Plaza2TradeSide::Sell;
    c.order_type = Plaza2TradeOrderType::Limit;
    c.price = "103000";
    c.quantity = 1;
    c.ext_id = 79;
    c.add_user_id = 701;
    c.cancel_user_id = 702;
    c.recovery_user_id = 703;
    c.run_id = "host-test";
    c.journal_root = f.root / "journals";
    c.add_observation_timeout = std::chrono::seconds(2);
    c.cancel_observation_timeout = std::chrono::seconds(2);
    c.max_poll_attempts = 4;
    config.transport.execution_safety_receipt_path = f.root / "receipt.json";
    return config;
}

void fresh(ConnectorHost& host) {
    const auto error = host.start();
    test::require(!error, "restart startup: " + error.message);
    for (int i = 0; i < 15; ++i)
        test::require(!host.poll(), "fresh bootstrap");
    test::require(host.snapshot().private_streams_ready, "fresh private streams");
}
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}
template <class Function> void crash(Function fn, int expected = 73) {
    const auto pid = ::fork();
    test::require(pid >= 0, "fork");
    if (pid == 0) {
        try {
            fn();
            ::_exit(73);
        } catch (const std::exception& e) {
            std::cerr << e.what() << std::endl;
            ::_exit(2);
        }
    }
    int status{};
    test::require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == expected,
                  "deterministic child crash boundary");
}
} // namespace
int main(int argc, char** argv) {
    try {
        test::require(argc == 2, "fake runtime path");
        auto root = test::make_temp_directory("restart_recovery");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test"));
        ::setenv("HOST_TEST_ENV", "ini=config/t1.ini;key=00000000", 1);
        ::setenv("HOST_TEST_BROKER", "BRK1", 1);
        ::setenv("HOST_TEST_CLIENT", "C01", 1);
        ::setenv("MOEX_PLAZA2_TEST_CREDENTIALS", "test-only-secret", 1);
        ::setenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY", "00000000", 1);
        for (const auto* flag : {"MOEX_FAKE_ZERO_POSITION", "MOEX_FAKE_MISSING_ORDER", "MOEX_FAKE_FLAT_TRADE_REPLAY",
                                 "MOEX_FAKE_PERSISTENT_ORDER_SESSION", "MOEX_FAKE_REGULAR_RECOVERED_ORDER"})
            ::setenv(flag, "1", 1);
        ::setenv("MOEX_FAKE_CLIENT_CODE", "BRK1C01", 1);
        ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
        ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
        for (int stage = 0; stage < 9; ++stage) {
            auto c = config_for(fixture);
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "crash-" + std::to_string(stage);
            c.order.journal_root = root / c.order.run_id;
            const auto artifact = root / (c.order.run_id + "-old.json");
            // Real process exit without destructors at each durable boundary.
            crash([&] {
                ConnectorHost host(c);
                fresh(host);
                const auto plan =
                    host.plan_order(ConnectorHostOrderRequest{.side = c.order.side,
                                                              .price = c.order.price,
                                                              .base_contract_code = c.order.base_contract_code,
                                                              .comment = c.order.comment,
                                                              .quantity = c.order.quantity});
                test::require(plan.ok && !host.begin_order(plan.canonical_json, plan.sha256), "initial authority");
                if (stage == 7)
                    return; // authorized checkpoint, no submission
                test::require(host.submit_order().add_submission.post_invoked, "one Add");
                if (stage != 0)
                    (void)host.poll_order();
                if (stage == 8)
                    test::require(host.cancel_current_order().cancel_submission.post_invoked,
                                  "normal Cancel before crash");
            });
            if (stage > 1 && stage < 7) {
                crash([&] {
                    ConnectorHost host(c);
                    fresh(host);
                    test::require(host.snapshot().publisher_calls.post == 0, "restart automatic posts zero");
                    const auto plan = host.prepare_recovered_cancel(artifact);
                    test::require(plan.eligible(), "restart exact Working: " + plan.error);
                    if (stage == 2)
                        return;       // A: artifact prepared, not consumed
                    if (stage == 3) { // B: same durable consumption primitive, publisher not invoked
                        std::string error;
                        test::require(consume_recovered_artifact(artifact, plan.canonical_json, error), error);
                        return;
                    }
                    if (stage == 4)
                        ::setenv("MOEX_FAKE_EXIT_AFTER_MSGNEW", "1", 1); // C
                    if (stage == 5)
                        ::setenv("MOEX_FAKE_EXIT_AFTER_POST", "1", 1); // D
                    test::require(host.cancel_recovered_order(artifact, plan.sha256).cancel_submission.post_invoked,
                                  "one explicit cancel");
                    if (stage == 6) { // E: reply observed, private order still Working
                        const auto result = host.poll_order();
                        test::require(result.cancel_reply.has_value() && !result.market_safe_terminal,
                                      "reply without terminal evidence");
                    }
                });
            }
            const auto checkpoint_before = read(c.order.journal_root / "persistent_session.json");
            test::require(checkpoint_before.find("persistent_session.v2") != std::string::npos, "v2 checkpoint");
            {
                auto wrong = c;
                wrong.order.profile_fingerprint = std::string(64, 'f');
                ConnectorHost host(wrong);
                test::require(bool(host.start()), "wrong profile blocks restart");
            }
            {
                auto wrong = c;
                ++wrong.transport.target_session_id;
                ConnectorHost host(wrong);
                test::require(bool(host.start()), "session never retargeted");
            }
            {
                auto wrong = c;
                wrong.order.client_code = "C02";
                ConnectorHost host(wrong);
                test::require(bool(host.start()), "wrong exact account blocked");
            }
            {
                auto wrong = c;
                wrong.order.price = "103250";
                ConnectorHost host(wrong);
                test::require(bool(host.start()), "wrong price blocked");
            }
            if (stage == 0) {
                const auto checkpoint_path = c.order.journal_root / "persistent_session.json";
                auto bytes = read(checkpoint_path);
                auto v1 = bytes;
                const auto schema_at = v1.find("persistent_session.v2");
                v1.replace(schema_at, std::string("persistent_session.v2").size(), "persistent_session.v1");
                test::write_text_file(checkpoint_path, v1);
                {
                    ConnectorHost old(c);
                    test::require(bool(old.start()), "v1 nonterminal remains fail-closed");
                }
                auto idle_v1 = v1;
                const auto phase_at = idle_v1.find("add_may_have_been_sent");
                idle_v1.replace(phase_at, std::string("add_may_have_been_sent").size(), "idle");
                test::write_text_file(checkpoint_path, idle_v1);
                {
                    ConnectorHost old(c);
                    test::require(bool(old.start()), "v1 idle cannot reset recovered reply reservations");
                }
                test::write_text_file(checkpoint_path, bytes);
                std::filesystem::rename(checkpoint_path, checkpoint_path.string() + ".saved");
                {
                    ConnectorHost missing(c);
                    test::require(bool(missing.start()), "deleting checkpoint cannot restore Add");
                }
                std::filesystem::rename(checkpoint_path.string() + ".saved", checkpoint_path);
                const auto field = std::string("\"next_recovered_user_id\": ");
                auto exhausted = bytes;
                const auto begin = exhausted.find(field) + field.size();
                exhausted.replace(begin, exhausted.find(',', begin) - begin, "703");
                test::write_text_file(checkpoint_path, exhausted);
                {
                    ConnectorHost blocked(c);
                    fresh(blocked);
                    test::require(!blocked.prepare_recovered_cancel(root / "exhausted.json").eligible() &&
                                      blocked.snapshot().publisher_calls.post == 0,
                                  "reply identifier exhaustion blocks");
                }
                test::write_text_file(checkpoint_path, bytes);
            }
            // Independent restart attempts never use the checkpoint as exchange truth.
            for (const auto* flag : {"MOEX_FAKE_MISSING_TRADE_ORDER", "MOEX_FAKE_TRADE_IDENTITY_CONFLICT"}) {
                ::setenv(flag, "1", 1);
                {
                    ConnectorHost blocked(c);
                    fresh(blocked);
                    const auto match = blocked.reconcile_recovered_order();
                    test::require(match.outcome != RecoveredOrderOutcome::ExactlyOneWorkingMatch &&
                                      match.outcome != RecoveredOrderOutcome::TerminalAlready &&
                                      blocked.snapshot().publisher_calls.post == 0,
                                  "absence/conflict cannot resolve or send");
                }
                ::unsetenv(flag);
            }
            // Prove full fill from fresh exact TRADE data in a child so the main
            // Working branch and original checkpoint stay available for each crash case.
            crash([&] {
                ::setenv("MOEX_FAKE_RESTART_FILLED", "1", 1);
                ConnectorHost filled(c);
                fresh(filled);
                const auto match = filled.reconcile_recovered_order();
                test::require(match.outcome == RecoveredOrderOutcome::TerminalAlready && match.observation &&
                                  match.observation->state == OrderLifecycleState::Filled &&
                                  filled.snapshot().publisher_calls.post == 0,
                              "fresh full fill closes without Cancel");
                // Do not advance the shared checkpoint: this branch tests fresh proof only.
            });
            ConnectorHost host(c);
            fresh(host);
            test::require(
                !host.snapshot().new_order_allowed &&
                    !host.plan_order(ConnectorHostOrderRequest{.side = c.order.side,
                                                               .price = c.order.price,
                                                               .base_contract_code = c.order.base_contract_code,
                                                               .comment = c.order.comment,
                                                               .quantity = c.order.quantity})
                         .ok &&
                    !host.submit_order().add_submission.post_invoked,
                "recovery-only has no Add");
            test::require(!host.cancel_current_order().cancel_submission.post_invoked, "normal Cancel unavailable");
            const auto initial_posts = host.snapshot().publisher_calls.post;
            test::require(initial_posts == 0, "bootstrap and denied commands post zero");
            if (std::filesystem::exists(artifact)) {
                const auto old = read(artifact);
                test::require(
                    !host.cancel_recovered_order(artifact, cg::plaza2_sha256_hex(old)).cancel_submission.post_invoked,
                    "old process artifact rejected");
            }
            const auto plan = host.prepare_recovered_cancel(root / (c.order.run_id + "-new.json"));
            test::require(plan.eligible(), "new process exact fresh match: " + plan.error);
            test::require(plan.canonical_json.find("checkpoint_sha256") != std::string::npos &&
                              plan.canonical_json.find("process_identity") != std::string::npos,
                          "restart context binding");
            if (std::filesystem::exists(artifact)) {
                const auto old = read(artifact);
                const auto marker = std::string("\"user_id\":");
                const auto at = old.find(marker);
                const auto end = old.find(',', at);
                test::require(plan.canonical_json.find(old.substr(at, end - at)) == std::string::npos,
                              "reply reservation survives process exit");
            }
            ::setenv("MOEX_FAKE_CANCEL_AFTER_DEL", "1", 1);
            test::require(host.cancel_recovered_order(plan.artifact, plan.sha256).cancel_submission.post_invoked,
                          "only exact fresh approval allows one Cancel");
            for (int i = 0; i < 5 && !host.snapshot().market_safe; ++i)
                (void)host.poll_order();
            test::require(host.snapshot().market_safe && host.snapshot().publisher_calls.post == 1,
                          "terminal without Add or repeated Cancel");
            test::require(!host.finish_order_epoch() && !host.snapshot().new_order_allowed,
                          "recovery owner stays no-Add");
            test::require(!host.stop(), "resolved recovery shutdown");
            ::unsetenv("MOEX_FAKE_CANCEL_AFTER_DEL");
        }
        std::cout << "restart recovery crash windows A-E and identity/approval guards passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
