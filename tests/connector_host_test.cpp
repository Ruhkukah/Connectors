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
        test::require(reset && count, "independent fake counters");
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
        dlclose(library);
        test::remove_tree(root);
        std::cout << "connector host scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
