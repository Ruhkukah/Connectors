#include "moex/connector_host/operator_config.hpp"
#include "plaza2_runtime_test_support.hpp"

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

void warm(ConnectorHost& host) {
    test::require(!host.start(), "host start");
    for (unsigned i = 0; i < 10 && !host.snapshot().observation_ready; ++i)
        test::require(!host.poll(), "host poll");
    if (!host.snapshot().observation_ready)
        std::cerr << render_snapshot(host.snapshot(), true);
    test::require(host.snapshot().observation_ready, "host ready");
}
} // namespace

int main(int argc, char** argv) {
    try {
        test::require(argc == 2 || argc == 3, "fake runtime path [fixture output]");
        auto root = argc == 3 ? std::filesystem::path(argv[2]) : test::make_temp_directory("connector_host");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test"));
        ::setenv("HOST_TEST_ENV", "ini=config/t1.ini;key=00000000", 1);
        ::setenv("HOST_TEST_BROKER", "BRK1", 1);
        ::setenv("HOST_TEST_CLIENT", "C01", 1);
        ::setenv("MOEX_PLAZA2_TEST_CREDENTIALS", "test-only-secret", 1);
        ::setenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY", "00000000", 1);
        ::setenv("MOEX_FAKE_ZERO_POSITION", "1", 1);
        ::setenv("MOEX_FAKE_MISSING_ORDER", "1", 1);
        ::setenv("MOEX_FAKE_CLIENT_CODE", "BRK1C01", 1);
        ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
        if (argc == 3) {
            ConnectorHost host(config_for(fixture));
            warm(host);
            const auto plan = host.plan();
            test::require(plan.ok, "CLI canonical plan fixture");
            test::write_text_file(root / "canonical_plan.json", plan.canonical_json);
            test::write_text_file(root / "plan.sha256", plan.sha256);
            test::require(!host.stop(), "fixture host stop");
            return 0;
        }
        void* library = dlopen(fixture.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        test::require(library != nullptr, "load fake");
        auto reset = reinterpret_cast<void (*)()>(dlsym(library, "moex_fake_reset_publisher_counts"));
        auto count = reinterpret_cast<std::uint64_t (*)(std::uint32_t)>(dlsym(library, "moex_fake_publisher_count"));
        auto env_open_count = reinterpret_cast<std::uint64_t (*)()>(dlsym(library, "moex_fake_environment_open_count"));
        auto connection_new_count =
            reinterpret_cast<std::uint64_t (*)()>(dlsym(library, "moex_fake_connection_new_count"));
        test::require(reset && count && env_open_count && connection_new_count, "independent fake counters");
        {
            reset();
            ConnectorHost host(config_for(fixture));
            test::require(host.snapshot().state == ConnectorHostState::Created && !host.snapshot().observation_ready,
                          "created snapshot");
            test::require(static_cast<bool>(host.poll()), "poll before start refused");
            warm(host);
            test::require(static_cast<bool>(host.start()), "double start refused");
            auto s = host.snapshot();
            test::require(s.state == ConnectorHostState::Ready && s.private_streams_ready &&
                              s.target_refdata_provenance_ready &&
                              s.fut_instruments_provenance.lifenum == s.refdata_lifenum &&
                              s.fut_sess_contents_provenance.lifenum == s.refdata_lifenum &&
                              s.session_provenance.lifenum == s.refdata_lifenum && s.trade_anchor &&
                              s.trade_anchor->trades_rev == s.pos_trades_rev &&
                              s.trade_anchor->trades_lifenum == s.pos_trades_lifenum && s.trade_replay_complete &&
                              s.uob_periodic_consistent && s.zero_starting_position_proven,
                          "typed evidence preserved");
            const auto plan = host.plan();
            test::require(plan.ok, "plan from current evidence: " + plan.message);
            test::require(static_cast<bool>(host.authorize(plan.canonical_json, plan.sha256)),
                          "qualify cannot authorize");
            test::require(!host.submit().ok && count(0) == 0 && count(1) == 0, "qualify cannot allocate/post");
            test::require(render_snapshot(s, true).find("test-only-secret") == std::string::npos, "privacy");
            test::require(!host.stop() && !host.stop() && host.snapshot().state == ConnectorHostState::Stopped &&
                              !host.snapshot().observation_ready,
                          "stop is idempotent and readiness clears");
        }
        {
            auto c = config_for(fixture);
            c.transport.host.arm_state.test_order_send_armed = true;
            ConnectorHost host(std::move(c));
            test::require(static_cast<bool>(host.start()), "qualify rejects send arm before start");
        }
        for (const char* flag : {"MOEX_FAKE_NONTRADABLE_SESSION", "MOEX_FAKE_NONTRADABLE_INSTRUMENT",
                                 "MOEX_FAKE_AGGR_CROSSED", "MOEX_FAKE_MISSING_LIMITS"}) {
            reset();
            ::setenv(flag, "1", 1);
            ConnectorHost host(config_for(fixture));
            test::require(!host.start(), "negative readiness start");
            for (int i = 0; i < 6; ++i)
                test::require(!host.poll(), "negative readiness poll");
            test::require(!host.snapshot().observation_ready && !host.plan().ok && !host.submit().ok && count(0) == 0 &&
                              count(1) == 0,
                          "readiness gate is not weakened");
            test::require(!host.stop(), "negative readiness stop");
            ::unsetenv(flag);
        }
        {
            ::setenv("MOEX_FAKE_MISSING_POSITION", "1", 1);
            ::setenv("MOEX_FAKE_FLAT_TRADE_REPLAY", "1", 1);
            ConnectorHost host(config_for(fixture));
            warm(host);
            test::require(host.snapshot().position_evidence_class ==
                              PositionEvidenceClass::FlatByPosSnapshotAndTradeReplay,
                          "sparse POS uses existing anchored replay classification");
            test::require(!host.stop(), "sparse POS stop");
            ::unsetenv("MOEX_FAKE_MISSING_POSITION");
            ::unsetenv("MOEX_FAKE_FLAT_TRADE_REPLAY");
        }
        for (int scenario = 0; scenario < 4; ++scenario) {
            reset();
            ::unsetenv("MOEX_FAKE_FULL_FILL");
            ::unsetenv("MOEX_FAKE_PUB_REPLY_TIMEOUT_ADD_ONLY");
            ::unsetenv("MOEX_FAKE_CANCEL_AFTER_RECOVERY");
            ::unsetenv("MOEX_FAKE_TRADE_IDENTITY_CONFLICT");
            ::setenv("MOEX_FAKE_CANCEL_AFTER_DEL", "1", 1);
            if (scenario == 1)
                ::setenv("MOEX_FAKE_FULL_FILL", "1", 1);
            if (scenario == 2) {
                ::setenv("MOEX_FAKE_PUB_REPLY_TIMEOUT_ADD_ONLY", "1", 1);
                ::setenv("MOEX_FAKE_CANCEL_AFTER_RECOVERY", "1", 1);
            }
            if (scenario == 3)
                ::setenv("MOEX_FAKE_TRADE_IDENTITY_CONFLICT", "1", 1);
            auto c = config_for(fixture);
            c.order.run_id += std::to_string(scenario);
            c.order.journal_root = fixture.root / ("journal" + std::to_string(scenario));
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            const auto journal_root = c.order.journal_root;
            ConnectorHost host(std::move(c));
            warm(host);
            const auto plan = host.plan();
            test::require(plan.ok, "current plan valid");
            test::require(static_cast<bool>(host.authorize(plan.canonical_json, std::string(64, '0'))),
                          "wrong SHA refused");
            const auto auth = host.authorize(plan.canonical_json, plan.sha256);
            test::require(!auth, "exact authorization: " + auth.message);
            const auto result = host.submit();
            if (!result.ok)
                std::cerr << "scenario=" << scenario << " " << result.message << '\n';
            if (scenario == 3) {
                test::require(!result.ok && !result.market_safe_terminal && !result.evidence_consistent &&
                                  result.state == OrderLifecycleState::UnresolvedOrphanIncident,
                              "identity conflict fail closed");
                for (const auto* name : {"ext_79", "user_701", "user_702", "user_703"})
                    test::require(std::filesystem::is_directory(journal_root / "active" / name),
                                  "all unsafe identifier locks retained");
                std::ifstream journal(result.journal_path);
                const std::string content(std::istreambuf_iterator<char>(journal), {});
                test::require(content.find("unresolved_orphan_incident") != std::string::npos,
                              "unresolved state persisted");
            } else {
                test::require(result.ok && result.market_safe_terminal && result.evidence_consistent &&
                                  result.state ==
                                      (scenario == 1 ? OrderLifecycleState::Filled : OrderLifecycleState::Cancelled),
                              "full lifecycle");
                test::require(count(0) == (scenario == 1 ? 1U : 2U) && count(1) == (scenario == 1 ? 1U : 2U),
                              "no extra order");
                test::require(result.recovery_submission.post_invoked == (scenario == 2), "uncertainty recovery only");
            }
            const auto before = count(1);
            test::require(!host.submit().ok && count(1) == before, "no second Add");
            test::require(!host.stop(), "terminal stop");
        }
        {
            reset();
            ::unsetenv("MOEX_FAKE_TRADE_IDENTITY_CONFLICT");
            ::unsetenv("MOEX_FAKE_FULL_FILL");
            ::unsetenv("MOEX_FAKE_CANCEL_AFTER_RECOVERY");
            ::setenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION", "1", 1);
            ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
            ::setenv("MOEX_FAKE_CANCEL_AFTER_DEL", "1", 1);
            ::setenv("MOEX_FAKE_FLAT_TRADE_REPLAY", "1", 1);
            auto c = config_for(fixture);
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "persistent-host";
            c.order.journal_root = fixture.root / "persistent-journals";
            ConnectorHost host(std::move(c));
            warm(host);
            test::require(env_open_count() == 1 && connection_new_count() == 1, "one warm CGate session");

            const ConnectorHostOrderRequest first_request{.side = Plaza2TradeSide::Sell,
                                                          .price = "103000",
                                                          .base_contract_code = "RTS",
                                                          .comment = "persistent-sell-a",
                                                          .quantity = 1};
            const ConnectorHostOrderRequest second_request{.side = Plaza2TradeSide::Buy,
                                                           .price = "102250",
                                                           .base_contract_code = "RTS",
                                                           .comment = "persistent-buy-b",
                                                           .quantity = 1};
            const ConnectorHostOrderRequest third_request{.side = Plaza2TradeSide::Sell,
                                                          .price = "103250",
                                                          .base_contract_code = "RTS",
                                                          .comment = "persistent-sell-c",
                                                          .quantity = 1};
            const auto first_plan = host.plan_order(first_request);
            test::require(first_plan.ok, "persistent first plan");
            test::require(
                static_cast<bool>(host.begin_order(first_request, first_plan.canonical_json, std::string(64, '0'))) &&
                    !host.snapshot().order_epoch_active && count(0) == 0 && count(1) == 0,
                "wrong persistent authorization opens no epoch");
            test::require(!host.begin_order(first_request, first_plan.canonical_json, first_plan.sha256),
                          "persistent first authorization");
            test::require(host.snapshot().order_epoch_active && host.snapshot().order_authorized &&
                              !host.snapshot().order_submission_attempted,
                          "authorized epoch snapshot");
            auto first_submit = host.submit_order();
            test::require(!first_submit.ok && first_submit.state == OrderLifecycleState::Posted,
                          "persistent first Add is not terminal");
            const auto posts_after_first_add = count(1);
            test::require(!host.submit_order().ok && count(1) == posts_after_first_add,
                          "persistent second Add in one epoch is refused");
            test::require(
                static_cast<bool>(host.begin_order(first_request, first_plan.canonical_json, first_plan.sha256)),
                "new order while Add is pending is refused");
            auto first_working = host.poll_order();
            test::require(!first_working.ok && first_working.state == OrderLifecycleState::Working,
                          "persistent first order returns Working state=" +
                              std::string(order_lifecycle_state_name(first_working.state)) +
                              " message=" + first_working.message);
            const auto posts_before_cancel = count(1);
            test::require(host.poll_order().state == OrderLifecycleState::Working && count(1) == posts_before_cancel,
                          "persistent poll does not cancel automatically");
            const auto first_cancel = host.cancel_current_order();
            test::require(!first_cancel.ok && first_cancel.state == OrderLifecycleState::CancelPending,
                          "persistent explicit cancel pending");
            const auto posts_after_first_cancel = count(1);
            test::require(!host.cancel_current_order().ok && count(1) == posts_after_first_cancel,
                          "persistent second cancel in one epoch is refused");
            OrderLifecycleResult first_terminal;
            for (int attempt = 0; attempt < 4 && first_terminal.state != OrderLifecycleState::Cancelled; ++attempt)
                first_terminal = host.poll_order();
            test::require(first_terminal.ok && first_terminal.state == OrderLifecycleState::Cancelled,
                          "persistent first cancellation terminal");
            test::require(static_cast<bool>(host.stop()), "active epoch blocks host stop");
            test::require(!host.finish_order_epoch(), "first epoch closes safely");
            test::require(env_open_count() == 1 && connection_new_count() == 1 && host.snapshot().new_order_allowed,
                          "same warm session permits a fresh epoch");

            ::setenv("MOEX_FAKE_EXT_ID", "80", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20103", 1);
            const auto second_plan = host.plan_order(second_request);
            test::require(second_plan.ok && second_plan.sha256 != first_plan.sha256 &&
                              second_plan.canonical_json.find("persistent-buy-b") != std::string::npos &&
                              second_plan.canonical_json.find("102250") != std::string::npos &&
                              second_plan.canonical_json.find("buy") != std::string::npos,
                          "fresh epoch application terms change the canonical plan: " + second_plan.message);
            test::require(
                static_cast<bool>(host.begin_order(second_request, first_plan.canonical_json, first_plan.sha256)) &&
                    !host.snapshot().order_epoch_active,
                "old epoch authorization cannot open the next epoch");
            test::require(!host.begin_order(second_request, second_plan.canonical_json, second_plan.sha256),
                          "persistent second authorization");
            test::require(!host.submit_order().ok, "persistent second Add submitted");
            OrderLifecycleResult second_working;
            for (int attempt = 0; attempt < 3 && second_working.state != OrderLifecycleState::Working; ++attempt)
                second_working = host.poll_order();
            test::require(second_working.state == OrderLifecycleState::Working,
                          "persistent second order returns Working state=" +
                              std::string(order_lifecycle_state_name(second_working.state)) + " message=" +
                              second_working.message + " snapshot=" + render_snapshot(host.snapshot(), true));
            const auto second_still_working = host.poll_order();
            test::require(second_still_working.state == OrderLifecycleState::Working,
                          "persistent second order remains Working until cancel state=" +
                              std::string(order_lifecycle_state_name(second_still_working.state)));
            test::require(host.cancel_current_order().state == OrderLifecycleState::CancelPending,
                          "persistent second explicit cancel");
            OrderLifecycleResult second_terminal;
            for (int attempt = 0; attempt < 4 && second_terminal.state != OrderLifecycleState::Cancelled; ++attempt)
                second_terminal = host.poll_order();
            test::require(second_terminal.ok && second_terminal.state == OrderLifecycleState::Cancelled,
                          "persistent second cancellation terminal");
            test::require(!host.finish_order_epoch(), "second epoch closes safely");
            ::setenv("MOEX_FAKE_EXT_ID", "81", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20203", 1);
            ::setenv("MOEX_FAKE_FULL_FILL", "1", 1);
            const auto third_plan = host.plan_order(third_request);
            test::require(third_plan.ok && third_plan.sha256 != second_plan.sha256, "filled epoch plan authorization");
            test::require(!host.begin_order(third_request, third_plan.canonical_json, third_plan.sha256),
                          "persistent filled epoch authorization");
            test::require(!host.submit_order().ok, "persistent filled Add submitted");
            const auto third_terminal = host.poll_order();
            test::require(third_terminal.ok && third_terminal.state == OrderLifecycleState::Filled,
                          "persistent filled epoch terminal");
            test::require(!host.finish_order_epoch() && host.snapshot().new_order_allowed,
                          "filled epoch permits the next safe epoch");
            test::require(!host.stop(), "persistent warm host stop");
            ::unsetenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION");
            ::unsetenv("MOEX_FAKE_EXT_ID");
            ::unsetenv("MOEX_FAKE_PUB_REPLY_ORDER_ID");
            ::unsetenv("MOEX_FAKE_CANCEL_AFTER_DEL");
            ::unsetenv("MOEX_FAKE_FLAT_TRADE_REPLAY");
            ::unsetenv("MOEX_FAKE_FULL_FILL");
        }
        {
            // A cancel command is an application state.  A still-working
            // TRADE row must not make CancelPending regress to Working.
            reset();
            ::setenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION", "1", 1);
            ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
            ::unsetenv("MOEX_FAKE_CANCEL_AFTER_DEL");
            auto c = config_for(fixture);
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "cancel-pending-host";
            c.order.journal_root = fixture.root / "cancel-pending-journals";
            ConnectorHost host(std::move(c));
            warm(host);
            const ConnectorHostOrderRequest request{.side = Plaza2TradeSide::Sell,
                                                    .price = "103000",
                                                    .base_contract_code = "RTS",
                                                    .comment = "cancel-pending",
                                                    .quantity = 1};
            const auto plan = host.plan_order(request);
            test::require(plan.ok && !host.begin_order(request, plan.canonical_json, plan.sha256),
                          "cancel-pending epoch authorization");
            const auto cancel_pending_submit = host.submit_order();
            const auto cancel_pending_working = host.poll_order();
            test::require(!cancel_pending_submit.ok &&
                              (cancel_pending_working.state == OrderLifecycleState::Working ||
                               cancel_pending_working.state == OrderLifecycleState::PartiallyFilled),
                          "cancel-pending order reaches Working: submit=" + cancel_pending_submit.message +
                              " poll=" + cancel_pending_working.message +
                              " state=" + std::string(order_lifecycle_state_name(cancel_pending_working.state)));
            test::require(host.cancel_current_order().state == OrderLifecycleState::CancelPending,
                          "explicit cancel enters CancelPending");
            const auto still_pending = host.poll_order();
            test::require(!still_pending.ok && still_pending.state == OrderLifecycleState::CancelPending,
                          "Working TRADE evidence does not regress CancelPending");
            ::setenv("MOEX_FAKE_FORCE_TRADE_TERMINAL", "1", 1);
            const auto cancelled = host.poll_order();
            test::require(cancelled.ok && cancelled.state == OrderLifecycleState::Cancelled,
                          "cancel-pending epoch eventually reaches terminal evidence");
            test::require(!host.finish_order_epoch(), "cancel-pending epoch finishes safely");
            ::unsetenv("MOEX_FAKE_FORCE_TRADE_TERMINAL");
        }
        {
            // A definitive non-timeout cancel rejection while TRADE remains
            // Working is an unresolved orphan, not a second-cancel invitation.
            reset();
            ::setenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION", "1", 1);
            ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_REJECT_DEL", "1", 1);
            auto c = config_for(fixture);
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "cancel-rejected-host";
            c.order.journal_root = fixture.root / "cancel-rejected-journals";
            const auto journal_root = c.order.journal_root;
            ConnectorHost host(std::move(c));
            warm(host);
            const ConnectorHostOrderRequest request{.side = Plaza2TradeSide::Sell,
                                                    .price = "103000",
                                                    .base_contract_code = "RTS",
                                                    .comment = "cancel-rejected",
                                                    .quantity = 1};
            const auto plan = host.plan_order(request);
            test::require(plan.ok && !host.begin_order(request, plan.canonical_json, plan.sha256),
                          "cancel-rejected epoch authorization");
            const auto rejected_submit = host.submit_order();
            const auto rejected_working = host.poll_order();
            test::require(!rejected_submit.ok && (rejected_working.state == OrderLifecycleState::Working ||
                                                  rejected_working.state == OrderLifecycleState::PartiallyFilled),
                          "cancel-rejected order reaches Working");
            test::require(host.cancel_current_order().state == OrderLifecycleState::CancelPending,
                          "rejected cancel initially remains pending");
            const auto orphan = host.poll_order();
            test::require(!orphan.ok && !orphan.market_safe_terminal &&
                              orphan.state == OrderLifecycleState::UnresolvedOrphanIncident &&
                              !host.snapshot().new_order_allowed,
                          "definitive cancel rejection is fail-closed");
            for (const auto* name : {"ext_79", "user_701", "user_702", "user_703"})
                test::require(std::filesystem::is_directory(journal_root / "active" / name),
                              "definitive cancel rejection retains identifier locks");
            test::require(static_cast<bool>(host.finish_order_epoch()), "unsafe rejected-cancel epoch cannot finish");
            ::unsetenv("MOEX_FAKE_PUB_REPLY_REJECT_DEL");
        }
        {
            // A crash after the pre-send checkpoint must restore the exact
            // epoch identity and block a second Add until reconciliation.
            reset();
            ::setenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION", "1", 1);
            ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
            ::unsetenv("MOEX_FAKE_FULL_FILL");
            auto c = config_for(fixture);
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "restart-persistent-host";
            c.order.journal_root = fixture.root / "restart-persistent-journals";
            const auto journal_root = c.order.journal_root;
            const ConnectorHostOrderRequest request{.side = Plaza2TradeSide::Sell,
                                                    .price = "103000",
                                                    .base_contract_code = "RTS",
                                                    .comment = "restart-epoch",
                                                    .quantity = 1};
            {
                ConnectorHost host(std::move(c));
                warm(host);
                const auto plan = host.plan_order(request);
                test::require(plan.ok && !host.begin_order(request, plan.canonical_json, plan.sha256),
                              "restart epoch authorization");
                const auto restart_submit = host.submit_order();
                const auto restart_working = host.poll_order();
                test::require(!restart_submit.ok && (restart_working.state == OrderLifecycleState::Working ||
                                                     restart_working.state == OrderLifecycleState::PartiallyFilled),
                              "restart epoch reaches Working");
                test::require(std::filesystem::is_directory(journal_root / "active" / "ext_79"),
                              "restart epoch retains ext lock before destruction");
                std::ifstream checkpoint(journal_root / "persistent_session.json");
                const std::string checkpoint_text(std::istreambuf_iterator<char>(checkpoint), {});
                test::require(checkpoint &&
                                  checkpoint_text.find("\"phase\": \"add_may_have_been_sent\"") != std::string::npos &&
                                  checkpoint_text.find("\"ext_id\": 79") != std::string::npos &&
                                  checkpoint_text.find("\"comment\": \"restart-epoch\"") != std::string::npos &&
                                  checkpoint_text.find("\"plan_sha256\": \"") != std::string::npos,
                              "restart checkpoint records the exact active epoch terms and hashes");
            }
            // Use the same original base configuration and journal root.
            // Rebuild the exact restart configuration explicitly.
            {
                auto restart_config = config_for(fixture);
                restart_config.purpose = HostPurpose::OrderTest;
                restart_config.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
                restart_config.transport.host.arm_state.test_order_send_armed = true;
                restart_config.order.run_id = "restart-persistent-host";
                restart_config.order.journal_root = journal_root;
                ConnectorHost restarted(std::move(restart_config));
                test::require(restarted.snapshot().order_epoch_active && !restarted.snapshot().new_order_allowed,
                              "restart restores the active epoch checkpoint");
                const auto restart_start = restarted.start();
                test::require(!restart_start, "restart host starts for reconciliation: " + restart_start.message);
                for (int i = 0; i < 8 && !restarted.snapshot().observation_ready; ++i)
                    test::require(!restarted.poll(), "restart host poll");
                const auto blocked_plan = restarted.plan_order(request);
                test::require(!blocked_plan.ok && static_cast<bool>(restarted.begin_order(request, "", "")),
                              "restart blocks plan and begin before reconciliation");
                const auto unresolved = restarted.reconcile();
                test::require(unresolved.run_found && !unresolved.resolved && restarted.snapshot().order_epoch_active,
                              "Working restart evidence remains unresolved and locked");
            }
            ::setenv("MOEX_FAKE_FULL_FILL", "1", 1);
            auto terminal_config = config_for(fixture);
            terminal_config.purpose = HostPurpose::OrderTest;
            terminal_config.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            terminal_config.transport.host.arm_state.test_order_send_armed = true;
            terminal_config.order.run_id = "restart-persistent-host";
            terminal_config.order.journal_root = journal_root;
            ConnectorHost terminal_host(std::move(terminal_config));
            test::require(!terminal_host.start(), "terminal restart host starts");
            for (int i = 0; i < 8 && !terminal_host.snapshot().observation_ready; ++i)
                test::require(!terminal_host.poll(), "terminal restart host poll");
            const auto resolved = terminal_host.reconcile();
            test::require(resolved.ok && resolved.resolved && !terminal_host.snapshot().order_epoch_active &&
                              terminal_host.snapshot().new_order_allowed,
                          "terminal restart evidence resolves the exact epoch: ok=" + std::to_string(resolved.ok) +
                              " resolved=" + std::to_string(resolved.resolved) + " msg=" + resolved.message +
                              " snap=" + render_snapshot(terminal_host.snapshot(), true));
            const auto next_plan = terminal_host.plan_order(request);
            test::require(next_plan.ok && next_plan.canonical_json.find("\"ext_id\": 80") != std::string::npos,
                          "post-restart next epoch uses advanced identifiers");
            test::require(!terminal_host.stop(), "restarted host stops after reconciliation");
            ::unsetenv("MOEX_FAKE_FULL_FILL");
        }
        {
            // If the process stops after publishing the pre-send checkpoint
            // but before a usable journal exists, the checkpoint remains a
            // conservative active epoch and cannot authorize another Add.
            reset();
            ::setenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION", "1", 1);
            ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
            auto c = config_for(fixture);
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "checkpoint-without-journal";
            c.order.journal_root = fixture.root / "checkpoint-without-journal";
            const auto journal_root = c.order.journal_root;
            const ConnectorHostOrderRequest request{.side = Plaza2TradeSide::Sell,
                                                    .price = "103000",
                                                    .base_contract_code = "RTS",
                                                    .comment = "checkpoint-only",
                                                    .quantity = 1};
            {
                ConnectorHost host(std::move(c));
                warm(host);
                const auto plan = host.plan_order(request);
                test::require(plan.ok && !host.begin_order(request, plan.canonical_json, plan.sha256),
                              "checkpoint-only epoch authorization");
                const auto submitted = host.submit_order();
                test::require(!submitted.ok && std::filesystem::exists(journal_root / "persistent_session.json"),
                              "checkpoint-only Add attempt publishes its checkpoint");
                std::error_code remove_error;
                std::filesystem::remove(journal_root / "checkpoint-without-journal" / "journal.json", remove_error);
                test::require(!remove_error, "checkpoint-only fixture removes the usable journal");
            }
            auto restart_config = config_for(fixture);
            restart_config.purpose = HostPurpose::OrderTest;
            restart_config.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            restart_config.transport.host.arm_state.test_order_send_armed = true;
            restart_config.order.run_id = "checkpoint-without-journal";
            restart_config.order.journal_root = journal_root;
            ConnectorHost restarted(std::move(restart_config));
            test::require(restarted.snapshot().order_epoch_active && !restarted.snapshot().new_order_allowed,
                          "checkpoint-only restart remains blocked");
            test::require(!restarted.start(), "checkpoint-only restart starts for reconciliation");
            for (int i = 0; i < 8 && !restarted.snapshot().observation_ready; ++i)
                test::require(!restarted.poll(), "checkpoint-only restart poll");
            const auto unresolved = restarted.reconcile();
            test::require(unresolved.run_found && !unresolved.resolved && unresolved.locks_retained &&
                              !restarted.snapshot().new_order_allowed && count(1) == 1,
                          "checkpoint without journal never authorizes a second Add");
            ::unsetenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION");
            ::unsetenv("MOEX_FAKE_EXT_ID");
            ::unsetenv("MOEX_FAKE_PUB_REPLY_ORDER_ID");
        }
        {
            // A timeout does not prove cancellation.  The epoch remains
            // CancelPending and cannot authorize another Add.
            reset();
            ::setenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION", "1", 1);
            ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_TIMEOUT_DEL", "1", 1);
            ::unsetenv("MOEX_FAKE_CANCEL_AFTER_DEL");
            auto c = config_for(fixture);
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "cancel-timeout-host";
            c.order.journal_root = fixture.root / "cancel-timeout-journals";
            ConnectorHost host(std::move(c));
            warm(host);
            const ConnectorHostOrderRequest request{.side = Plaza2TradeSide::Sell,
                                                    .price = "103000",
                                                    .base_contract_code = "RTS",
                                                    .comment = "cancel-timeout",
                                                    .quantity = 1};
            const auto plan = host.plan_order(request);
            test::require(plan.ok && !host.begin_order(request, plan.canonical_json, plan.sha256),
                          "cancel-timeout epoch authorization");
            static_cast<void>(host.submit_order());
            static_cast<void>(host.poll_order());
            test::require(host.cancel_current_order().state == OrderLifecycleState::CancelPending,
                          "timeout cancel enters CancelPending");
            const auto timeout = host.poll_order();
            test::require(!timeout.ok && timeout.state == OrderLifecycleState::CancelPending &&
                              !host.snapshot().new_order_allowed,
                          "cancel timeout remains unresolved and blocks a new epoch");
            ::unsetenv("MOEX_FAKE_PUB_REPLY_TIMEOUT_DEL");
        }
        {
            // Exact-ext recovery has the same fail-closed command semantics.
            reset();
            ::setenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION", "1", 1);
            ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
            ::setenv("MOEX_FAKE_MISSING_TRADE_ORDER", "1", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_REJECT_RECOVERY", "1", 1);
            auto c = config_for(fixture);
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "recovery-rejected-host";
            c.order.journal_root = fixture.root / "recovery-rejected-journals";
            const auto journal_root = c.order.journal_root;
            ConnectorHost host(std::move(c));
            warm(host);
            const ConnectorHostOrderRequest request{.side = Plaza2TradeSide::Sell,
                                                    .price = "103000",
                                                    .base_contract_code = "RTS",
                                                    .comment = "recovery-rejected",
                                                    .quantity = 1};
            const auto plan = host.plan_order(request);
            test::require(plan.ok && !host.begin_order(request, plan.canonical_json, plan.sha256),
                          "recovery-rejected epoch authorization");
            static_cast<void>(host.submit_order());
            static_cast<void>(host.poll_order());
            const auto recovery_pending = host.cancel_current_order();
            test::require(!recovery_pending.ok && recovery_pending.state == OrderLifecycleState::CancelPending,
                          "explicit cancel enters recovery CancelPending");
            const auto recovery_orphan = host.poll_order();
            test::require(!recovery_orphan.ok && !recovery_orphan.market_safe_terminal &&
                              recovery_orphan.state == OrderLifecycleState::UnresolvedOrphanIncident &&
                              std::filesystem::is_directory(journal_root / "active" / "ext_79"),
                          "definitive recovery rejection is fail-closed with locks retained");
            ::unsetenv("MOEX_FAKE_MISSING_TRADE_ORDER");
            ::unsetenv("MOEX_FAKE_PUB_REPLY_REJECT_RECOVERY");
        }
        ::unsetenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION");
        ::unsetenv("MOEX_FAKE_EXT_ID");
        ::unsetenv("MOEX_FAKE_PUB_REPLY_ORDER_ID");
        ::unsetenv("MOEX_FAKE_PUB_REPLY_REJECT_DEL");
        ::unsetenv("MOEX_FAKE_FORCE_TRADE_TERMINAL");
        dlclose(library);
        test::remove_tree(root);
        std::cout << "connector host scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
