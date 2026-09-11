#include "moex/connector_host/operator_config.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "plaza2_trade/fixtures/cgate99_messages.hpp"

#include <cstring>

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

struct QualificationObserver final : cg::Plaza2QualificationObserver, cg::Plaza2Aggr20QualificationObserver {
    std::size_t events{}, commits{}, forensic_rows{};
    bool forensic_identity{}, forensic_equal{true};
    bool wants_forensic_row(const cg::Plaza2ListenerEvent& e) const noexcept override {
        return e.table_code == moex::plaza2::generated::TableCode::kFortsRefdataReplFutSessContents;
    }
    void forensic_row(cg::Plaza2ForensicRow row) noexcept override {
        ++forensic_rows;
        for (const auto& f : row.fields) {
            forensic_equal &= f.equal && f.offset + f.size <= row.payload.size();
            if (f.name == "isin_id")
                forensic_identity = f.independent_value == "1001";
        }
    }
    void observe(const cg::Plaza2ListenerEvent&, const cg::Plaza2Error&) noexcept override {
        ++events;
    }
    void committed(const cg::Plaza2Aggr20Snapshot&) noexcept override {
        ++commits;
    }
};

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
        // Synthetic mixed-case identity through parser, observation, intent plan and wire encoder.
        std::vector<std::string> account_plans;
        for (const auto* client : {"12o", "12O", "120"}) {
            ::setenv("HOST_TEST_BROKER", "AbC9", 1);
            ::setenv("HOST_TEST_CLIENT", client, 1);
            const std::string full = std::string("AbC9") + client;
            ::setenv("MOEX_FAKE_CLIENT_CODE", full.c_str(), 1);
            const auto config = config_for(fixture);
            test::require(config.order.broker_code == "AbC9" && config.order.client_code == client &&
                              config.transport.observation_client_code == full,
                          "4+3 parser round-trip preserves case");
            ConnectorHost host(config);
            warm(host);
            test::require(host.snapshot().participant_identity_exact && host.snapshot().participant_limit_row_present,
                          "configured mixed-case PART identity matches");
            const auto plan = host.plan();
            test::require(plan.ok, "mixed-case intent plan");
            official_cgate99::AddOrder wire{};
            test::require(plan.add_command.payload.size() == sizeof(wire), "official AddOrder size");
            std::memcpy(&wire, plan.add_command.payload.data(), sizeof(wire));
            test::require(std::string(wire.broker_code) == "AbC9" && std::string(wire.client_code) == client,
                          "encoder preserves exact selected bytes");
            test::require(plan.canonical_json.find(cg::plaza2_sha256_hex(std::string_view(client))) !=
                              std::string::npos,
                          "intent binds exact client bytes");
            account_plans.push_back(plan.sha256);
            test::require(host.snapshot().publisher_calls.post == 0, "encoding test posts nothing");
            test::require(!host.stop(), "mixed-case observation stop");
        }
        test::require(account_plans[0] != account_plans[1] && account_plans[0] != account_plans[2] &&
                          account_plans[1] != account_plans[2],
                      "o O and 0 yield distinct intents");
        ::setenv("HOST_TEST_BROKER", "BRK1", 1);
        ::setenv("HOST_TEST_CLIENT", "C01", 1);
        ::setenv("MOEX_FAKE_CLIENT_CODE", "BRK1C01", 1);

        auto reset = reinterpret_cast<void (*)()>(dlsym(library, "moex_fake_reset_publisher_counts"));
        auto count = reinterpret_cast<std::uint64_t (*)(std::uint32_t)>(dlsym(library, "moex_fake_publisher_count"));
        auto env_open_count = reinterpret_cast<std::uint64_t (*)()>(dlsym(library, "moex_fake_environment_open_count"));
        auto connection_new_count =
            reinterpret_cast<std::uint64_t (*)()>(dlsym(library, "moex_fake_connection_new_count"));
        test::require(reset && count && env_open_count && connection_new_count, "independent fake counters");
        {
            auto config = config_for(fixture);
            config.transport.host.transport_recovery_enabled = false;
            ConnectorHost host(config);
            warm(host);
            test::require(host.snapshot().private_snapshot_state_ready && host.snapshot().aggr_ready,
                          "raw and effective states ready before loss");
            ::setenv("MOEX_FAKE_CONNECTION_ERROR", "1", 1);
            const auto failure = host.poll();
            ::unsetenv("MOEX_FAKE_CONNECTION_ERROR");
            const auto failed = host.snapshot();
            test::require(failure.code == cg::Plaza2ErrorCode::AdapterState && failure.runtime_code != 0 &&
                              failure.message.find("CG_ERR_INCORRECTSTATE") != std::string::npos,
                          "original runtime cause preserved");
            test::require(failed.private_snapshot_state_ready && failed.aggr_snapshot_state_ready &&
                              !failed.private_streams_ready && !failed.aggr_ready && !failed.publisher_ready &&
                              !failed.reply_ready && !failed.observation_ready && !failed.new_order_allowed,
                          "transport loss masks all effective readiness while retaining raw snapshots");
            test::require(failed.causal_error.message == failure.message &&
                              failed.causal_error.runtime_code == failure.runtime_code,
                          "snapshot retains causal error");
            test::require(!host.stop(), "stop failed host without orders");
        }
        {
            auto config = config_for(fixture);
            config.transport.host.transport_recovery_enabled = false;
            ConnectorHost host(config);
            warm(host);
            ::setenv("MOEX_FAKE_PRIVATE_ERROR_AFTER_READY", "1", 1);
            const auto failure = host.poll();
            ::unsetenv("MOEX_FAKE_PRIVATE_ERROR_AFTER_READY");
            const auto failed = host.snapshot();
            test::require(failure.code == cg::Plaza2ErrorCode::AdapterState && failure.runtime_code == 0,
                          "internally generated listener error is not a fabricated CGate result");
            test::require(failed.private_snapshot_state_ready && !failed.private_streams_ready && !failed.aggr_ready &&
                              !failed.publisher_ready && !failed.reply_ready && !failed.new_order_allowed,
                          "private ERROR masks every effective gate");
            test::require(!host.stop(), "stop listener-error host");
        }
        const auto timeout_polls = [&](ConnectorHost& host, bool ready) {
            const auto before = host.snapshot();
            const auto connections = connection_new_count();
            ::setenv("MOEX_FAKE_PROCESS_TIMEOUT", "1", 1);
            for (int poll = 0; poll < 1000; ++poll)
                test::require(!host.poll(), "poll timeout is a normal no-event iteration");
            ::unsetenv("MOEX_FAKE_PROCESS_TIMEOUT");
            const auto after = host.snapshot();
            test::require(after.state != ConnectorHostState::Failed && after.state != ConnectorHostState::Recovering &&
                              after.recovery.generation == before.recovery.generation &&
                              after.recovery.attempts == before.recovery.attempts &&
                              connection_new_count() == connections && after.publisher_calls.post == 0,
                          "1000 timeouts cannot fail, recreate a connection, retry or post");
            if (ready)
                test::require(after.private_streams_ready && after.aggr_ready && after.publisher_ready &&
                                  after.reply_ready && after.observation_ready,
                              "timeout alone does not invalidate previously valid effective readiness");
        };
        {
            auto now = std::chrono::steady_clock::now();
            auto c = config_for(fixture);
            c.transport.host.recovery_now = [&] { return now; };
            ConnectorHost host(c);
            test::require(!host.start(), "timeout bootstrap start");
            timeout_polls(host, false);
            for (int i = 0; i < 10 && !host.snapshot().observation_ready; ++i)
                test::require(!host.poll(), "bootstrap continues after timeouts");
            test::require(host.snapshot().observation_ready, "bootstrap reaches ready without restart");
            timeout_polls(host, true);
            ::setenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS", "1", 1);
            test::require(!host.poll(), "timeout test socket loss");
            ::unsetenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS");
            now += std::chrono::seconds(1);
            for (int i = 0; i < 10 && !host.snapshot().observation_ready; ++i)
                test::require(!host.poll(), "timeout test recovery");
            test::require(host.snapshot().observation_ready && host.snapshot().recovery.attempts == 1,
                          "full recovery precedes post-recovery timeouts");
            timeout_polls(host, true);
            test::require(!host.stop(), "stop timeout regressions");
        }
        for (const char* result : {"131073", "131074", "131076", "131077", "999999"}) {
            ConnectorHost host(config_for(fixture));
            warm(host);
            ::setenv("MOEX_FAKE_PROCESS_RESULT", result, 1);
            const auto error = host.poll();
            ::unsetenv("MOEX_FAKE_PROCESS_RESULT");
            test::require(error && host.snapshot().state == ConnectorHostState::Failed &&
                              host.snapshot().recovery.attempts == 0 &&
                              error.runtime_code == static_cast<unsigned>(std::stoul(result)),
                          "process allowlist excludes invalid, unsupported, more, incorrect state and unknown results");
            test::require(!host.stop(), "stop disallowed process result");
        }
        for (const auto* operation :
             {"MOEX_FAKE_ENV_OPEN_RESULT", "MOEX_FAKE_CONNECTION_CREATE_RESULT", "MOEX_FAKE_CONNECTION_OPEN_RESULT",
              "MOEX_FAKE_LISTENER_OPEN_RESULT", "MOEX_FAKE_PUBLISHER_OPEN_RESULT"}) {
            ConnectorHost host(config_for(fixture));
            ::setenv(operation, "131072", 1);
            const auto error = host.start();
            ::unsetenv(operation);
            test::require(error && host.snapshot().state == ConnectorHostState::Failed &&
                              host.snapshot().recovery.attempts == 0,
                          "initial bootstrap INTERNAL is always fatal");
            test::require(!host.stop(), "stop initial bootstrap internal");
        }
        for (const char* code : {"131072", "131073"}) {
            for (const auto* operation : {"MOEX_FAKE_ENV_OPEN_RESULT", "MOEX_FAKE_CONNECTION_CREATE_RESULT",
                                          "MOEX_FAKE_LISTENER_OPEN_RESULT", "MOEX_FAKE_PUBLISHER_OPEN_RESULT"}) {
                auto now = std::chrono::steady_clock::now();
                auto c = config_for(fixture);
                c.transport.host.recovery_now = [&] { return now; };
                ConnectorHost host(c);
                warm(host);
                ::setenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS", "1", 1);
                test::require(!host.poll(), "bootstrap exclusion socket loss");
                ::unsetenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS");
                ::setenv(operation, code, 1);
                now += std::chrono::seconds(1);
                auto error = host.poll();
                for (int i = 0; i < 10 && !error; ++i)
                    error = host.poll();
                ::unsetenv(operation);
                test::require(error && host.snapshot().state == ConnectorHostState::Failed &&
                                  host.snapshot().recovery.first_cause.runtime_code == 131072,
                              "non-connection-open bootstrap failures remain fatal during recovery");
                test::require(!host.stop(), "stop bootstrap exclusion");
            }
        }
        {
            ConnectorHost host(config_for(fixture));
            ::setenv("MOEX_FAKE_CONNECTION_OPEN_RESULT", "131073", 1);
            const auto error = host.start();
            ::unsetenv("MOEX_FAKE_CONNECTION_OPEN_RESULT");
            test::require(error && host.snapshot().state == ConnectorHostState::Failed &&
                              host.snapshot().recovery.attempts == 0 && error.runtime_code == 131073,
                          "initial connection-open INVALIDARGUMENT never retries");
            test::require(!host.stop(), "stop initial invalid argument");
        }
        {
            reset();
            auto now = std::chrono::steady_clock::now();
            auto c = config_for(fixture);
            c.transport.host.recovery_now = [&] { return now; };
            ConnectorHost host(c);
            warm(host);
            ::setenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS", "1", 1);
            test::require(!host.poll() && host.snapshot().causal_health.connection == 1 &&
                              host.snapshot().recovery.origin == Plaza2FailureOrigin::ConnectionProcess,
                          "September 11: process INTERNAL with connection ERROR");
            ::unsetenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS");
            ::setenv("MOEX_FAKE_CONNECTION_OPEN_RESULT", "131073", 1);
            now += std::chrono::seconds(1);
            const auto error = host.poll();
            ::unsetenv("MOEX_FAKE_CONNECTION_OPEN_RESULT");
            const auto result = host.snapshot();
            std::cout << "SEPTEMBER_11_REOPEN_REGRESSION returned_error=" << static_cast<bool>(error)
                      << " attempts=" << result.recovery.attempts
                      << " first=" << result.recovery.first_cause.runtime_code
                      << " current=" << result.recovery.cause.runtime_code << '\n';
            test::require(!error && result.state == ConnectorHostState::Recovering && result.recovery.attempts == 1 &&
                              result.recovery.first_cause.runtime_code == 131072 &&
                              result.recovery.cause.runtime_code == 131073 && count(1) == 0,
                          "September 11: previously opened identity retries INVALIDARGUMENT");
            test::require(!host.stop(), "stop September 11 regression");
        }
        {
            Plaza2TestSessionHost host(config_for(fixture).transport.host);
            test::require(!host.start() && !host.stop(), "first operational lifetime opens successfully");
            ::setenv("MOEX_FAKE_CONNECTION_OPEN_RESULT", "131073", 1);
            const auto error = host.start();
            ::unsetenv("MOEX_FAKE_CONNECTION_OPEN_RESULT");
            test::require(error.runtime_code == 131073 &&
                              host.recovery_status().operation == Plaza2SessionOperation::Failed &&
                              host.recovery_status().attempts == 0,
                          "previous lifetime cannot authorize initial INVALIDARGUMENT retry");
            test::require(!host.stop(), "stop second operational lifetime");
        }
        for (const bool change_ini : {false, true}) {
            reset();
            auto now = std::chrono::steady_clock::now();
            auto c = config_for(fixture);
            c.transport.host.recovery_now = [&] { return now; };
            const auto ini_path = fixture.config_dir / "t1.ini";
            std::ifstream ini_input(ini_path, std::ios::binary);
            const std::string original_ini((std::istreambuf_iterator<char>(ini_input)), {});
            ini_input.close();
            ConnectorHost host(c);
            warm(host);
            ::setenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS", "1", 1);
            test::require(!host.poll(), "identity mutation enters accepted recovery");
            ::unsetenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS");
            if (change_ini) {
                std::ofstream output(ini_path, std::ios::app);
                output << "\n; changed router endpoint/profile\n";
            } else {
                ::setenv("MOEX_PLAZA2_TEST_CREDENTIALS", "changed-invalid-identity", 1);
            }
            ::setenv("MOEX_FAKE_CONNECTION_OPEN_RESULT", "131073", 1);
            now += std::chrono::seconds(1);
            const auto error = host.poll();
            ::unsetenv("MOEX_FAKE_CONNECTION_OPEN_RESULT");
            ::setenv("MOEX_PLAZA2_TEST_CREDENTIALS", "test-only-secret", 1);
            {
                std::ofstream output(ini_path, std::ios::binary);
                output << original_ini;
            }
            test::require(error && error.runtime_code == 0 &&
                              error.message.find("connection identity changed") != std::string::npos &&
                              host.snapshot().state == ConnectorHostState::Failed &&
                              host.snapshot().recovery.attempts == 1 && count(1) == 0,
                          "wrong recovery configuration fails before invoking connection open");
            test::require(!host.stop(), "stop changed identity");
        }
        for (const char* result : {"131074", "131077", "999999"}) {
            auto now = std::chrono::steady_clock::now();
            auto c = config_for(fixture);
            c.transport.host.recovery_now = [&] { return now; };
            ConnectorHost host(c);
            warm(host);
            ::setenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS", "1", 1);
            test::require(!host.poll(), "connection-open exclusion socket loss");
            ::unsetenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS");
            ::setenv("MOEX_FAKE_CONNECTION_OPEN_RESULT", result, 1);
            now += std::chrono::seconds(1);
            const auto error = host.poll();
            ::unsetenv("MOEX_FAKE_CONNECTION_OPEN_RESULT");
            test::require(error && host.snapshot().state == ConnectorHostState::Failed &&
                              host.snapshot().recovery.origin == Plaza2FailureOrigin::ConnectionOpen &&
                              host.snapshot().recovery.attempts == 1,
                          "unsupported, incorrect-state and unknown connection-open results remain fatal");
            test::require(!host.stop(), "stop connection-open exclusion");
        }
        // Deterministic recovery clock: no sleeps and no exchange endpoint.
        for (const auto* fault : {"MOEX_FAKE_CONNECTION_INTERNAL_LOSS", "MOEX_FAKE_PROCESS_INTERNAL_ACTIVE",
                                  "MOEX_FAKE_PRIVATE_ERROR_AFTER_READY", "MOEX_FAKE_AGGR_ERROR_AFTER_READY",
                                  "MOEX_FAKE_PUBLISHER_ERROR", "MOEX_FAKE_REPLY_ERROR"}) {
            reset();
            auto now = std::chrono::steady_clock::now();
            auto c = config_for(fixture);
            c.transport.host.recovery_now = [&] { return now; };
            ConnectorHost host(c);
            warm(host);
            const auto generation = host.snapshot().recovery.generation;
            ::setenv("MOEX_FAKE_SINGLE_PRIVATE_ERROR", "1", 1);
            ::setenv(fault, "1", 1);
            test::require(!host.poll(), std::string("recoverable loss: ") + fault);
            ::unsetenv(fault);
            ::unsetenv("MOEX_FAKE_SINGLE_PRIVATE_ERROR");
            const auto lost = host.snapshot();
            test::require(lost.state == ConnectorHostState::Recovering && !lost.observation_ready &&
                              !lost.private_streams_ready && !lost.aggr_ready && !lost.publisher_ready &&
                              !lost.reply_ready && !lost.new_order_allowed && lost.causal_error,
                          "all effective gates close immediately and cause retained");
            if (std::string_view(fault) == "MOEX_FAKE_CONNECTION_INTERNAL_LOSS" ||
                std::string_view(fault) == "MOEX_FAKE_PROCESS_INTERNAL_ACTIVE") {
                const auto expected_state = std::string_view(fault) == "MOEX_FAKE_CONNECTION_INTERNAL_LOSS" ? 1U : 3U;
                test::require(lost.recovery.origin == Plaza2FailureOrigin::ConnectionProcess &&
                                  lost.causal_error.runtime_code == 131072 &&
                                  lost.causal_error.message == "cg_conn_process: CG_ERR_INTERNAL" &&
                                  lost.causal_health.connection == expected_state && !lost.recovery.state_query_cause &&
                                  lost.causal_callback_error.empty(),
                              "real socket-close process cause and immediate connection state retained");
            }
            for (int i = 0; i < 10; ++i)
                test::require(!host.poll(), "idle recovery poll");
            test::require(host.snapshot().recovery.attempts == 0, "no busy retries");
            ::setenv("MOEX_FAKE_FRESH_POS_ANCHOR", "1", 1);
            now += std::chrono::seconds(1);
            for (int i = 0; i < 10 && !host.snapshot().observation_ready; ++i)
                test::require(!host.poll(), "fresh bootstrap poll");
            ::unsetenv("MOEX_FAKE_FRESH_POS_ANCHOR");
            const auto fresh = host.snapshot();
            test::require(fresh.observation_ready && fresh.recovery.generation == generation + 1 &&
                              fresh.recovery.attempts == 1 && fresh.publisher_ready && fresh.reply_ready &&
                              fresh.aggr_ready && count(0) == 0 && count(1) == 0,
                          std::string("fresh readiness without publisher calls: ") + fault);
            test::require(fresh.pos_trades_rev == 91 && fresh.pos_trades_lifenum == 8 && fresh.trade_anchor &&
                              fresh.trade_anchor->trades_rev == 91 && fresh.trade_anchor->trades_lifenum == 8 &&
                              fresh.trade_replay_complete,
                          "replacement POS anchor belongs to fresh LifeNum");
            test::require(!host.stop(), "stop recovered host");
        }
        for (const auto* fault : {"MOEX_FAKE_CONN_GETSTATE_INTERNAL", "MOEX_FAKE_CONN_GETSTATE_INTERNAL_ONCE",
                                  "MOEX_FAKE_LSN_GETSTATE_INTERNAL", "MOEX_FAKE_PUB_GETSTATE_INTERNAL",
                                  "MOEX_FAKE_PROCESS_INVALID_ARGUMENT"}) {
            reset();
            auto now = std::chrono::steady_clock::now();
            auto c = config_for(fixture);
            c.transport.host.recovery_now = [&] { return now; };
            QualificationObserver observer;
            c.transport.host.runtime.qualification_observer = &observer;
            ConnectorHost host(c);
            warm(host);
            const auto connections = connection_new_count();
            ::setenv(fault, "1", 1);
            const auto error = host.poll();
            ::unsetenv(fault);
            const auto failed = host.snapshot();
            test::require(error && failed.state == ConnectorHostState::Failed && !failed.observation_ready &&
                              failed.recovery.attempts == 0,
                          "unrelated internal and invalid argument remain fatal");
            now += std::chrono::seconds(2);
            test::require(host.poll() && connection_new_count() == connections && count(1) == 0,
                          "fatal unrelated operation cannot reconnect or post");
            test::require(!host.stop(), "stop fatal operation test");
        }
        {
            auto c = config_for(fixture);
            ConnectorHost host(c);
            warm(host);
            ::setenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS", "1", 1);
            ::setenv("MOEX_FAKE_CONN_GETSTATE_INTERNAL", "1", 1);
            const auto error = host.poll();
            ::unsetenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS");
            ::unsetenv("MOEX_FAKE_CONN_GETSTATE_INTERNAL");
            const auto failed = host.snapshot();
            test::require(error && failed.state == ConnectorHostState::Failed && failed.recovery.attempts == 0 &&
                              failed.recovery.origin == Plaza2FailureOrigin::ConnectionState &&
                              failed.recovery.process_cause.runtime_code == 131072 &&
                              failed.recovery.process_cause.message == "cg_conn_process: CG_ERR_INTERNAL" &&
                              failed.recovery.state_query_cause.message == "cg_conn_getstate: CG_ERR_INTERNAL",
                          "failed immediate state query preserves both errors and fails closed");
            const auto json = render_snapshot(failed, true);
            test::require(json.find("connection_process_cause") != std::string::npos &&
                              json.find("cg_conn_getstate: CG_ERR_INTERNAL") != std::string::npos,
                          "both causal errors persist in qualification JSON");
            test::require(!host.stop(), "stop failed state-query test");
        }
        for (const auto code : {131072U, 131073U}) {
            for (const bool exhaust : {false, true}) {
                reset();
                auto now = std::chrono::steady_clock::now();
                auto c = config_for(fixture);
                c.transport.host.recovery_now = [&] { return now; };
                c.transport.host.recovery_deadline = std::chrono::seconds(5);
                ConnectorHost host(c);
                warm(host);
                const auto generation = host.snapshot().recovery.generation;
                ::setenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS", "1", 1);
                test::require(!host.poll(), "ready router loss enters recovery");
                ::unsetenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS");
                ::setenv("MOEX_FAKE_CONNECTION_OPEN_RESULT", std::to_string(code).c_str(), 1);
                for (int i = 0; i < 2; ++i) {
                    now += std::chrono::seconds(1);
                    test::require(!host.poll(), "failed reconnect bounded");
                    test::require(
                        host.snapshot().state == ConnectorHostState::Recovering &&
                            host.snapshot().recovery.origin == Plaza2FailureOrigin::ConnectionOpen &&
                            host.snapshot().recovery.cause.runtime_code == code &&
                            host.snapshot().recovery.generation == generation &&
                            host.snapshot().recovery.attempts == static_cast<unsigned>(i + 1) &&
                            !host.snapshot().private_streams_ready && !host.snapshot().aggr_ready &&
                            !host.snapshot().publisher_ready && !host.snapshot().reply_ready,
                        "September 11 regression: absent router reopen remains in the same bounded recovery episode");
                }
                for (int idle = 0; idle < 10; ++idle)
                    test::require(!host.poll(), "reopen retry does not busy-loop");
                test::require(host.snapshot().recovery.attempts == 2, "idle polls do not create attempts");
                ::unsetenv("MOEX_FAKE_CONNECTION_OPEN_RESULT");
                if (exhaust) {
                    now += std::chrono::seconds(3);
                    const auto last = host.snapshot().causal_error;
                    const auto error = host.poll();
                    test::require(error && error.message == last.message && error.runtime_code == last.runtime_code &&
                                      host.snapshot().state == ConnectorHostState::Failed &&
                                      !host.snapshot().observation_ready,
                                  "deadline fails with last causal error");
                    test::require(host.snapshot().recovery.deadline_exhausted &&
                                      host.snapshot().recovery.attempts == 2 &&
                                      host.snapshot().recovery.first_cause.runtime_code == 131072 &&
                                      host.snapshot().recovery.process_cause.runtime_code == 131072,
                                  "bounded deadline retains initial INTERNAL alongside final reopen cause");
                } else {
                    ::setenv("MOEX_FAKE_FRESH_POS_ANCHOR", "1", 1);
                    now += std::chrono::seconds(1);
                    for (int i = 0; i < 10 && !host.snapshot().observation_ready; ++i)
                        test::require(!host.poll(), "third reconnect succeeds");
                    test::require(host.snapshot().observation_ready && host.snapshot().recovery.attempts == 3,
                                  "two failed attempts then success");
                    ::unsetenv("MOEX_FAKE_FRESH_POS_ANCHOR");
                    const auto fresh = host.snapshot();
                    test::require(
                        fresh.recovery.generation == generation + 1 && fresh.private_streams_ready &&
                            fresh.aggr_ready && fresh.publisher_ready && fresh.reply_ready &&
                            fresh.pos_trades_rev == 91 && fresh.pos_trades_lifenum == 8 && fresh.trade_anchor &&
                            fresh.trade_anchor->trades_rev == 91 && fresh.trade_anchor->trades_lifenum == 8 &&
                            fresh.trade_replay_complete,
                        "third attempt creates fresh POS to TRADE, private, AGGR and publisher/reply readiness");
                }
                test::require(count(1) == 0, "reconnect never posts");
                test::require(!host.stop(), "stop retry test");
            }
        }
        {
            reset();
            auto now = std::chrono::steady_clock::now();
            auto c = config_for(fixture);
            c.transport.host.recovery_now = [&] { return now; };
            c.transport.host.recovery_deadline = std::chrono::seconds(4);
            ConnectorHost host(c);
            warm(host);
            ::setenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS", "1", 1);
            test::require(!host.poll(), "persistent INTERNAL enters recovery");
            for (int attempt = 1; attempt <= 3; ++attempt) {
                now += std::chrono::seconds(1);
                test::require(!host.poll() && host.snapshot().state == ConnectorHostState::Recovering &&
                                  host.snapshot().recovery.attempts == static_cast<unsigned>(attempt),
                              "reopened connection fails process without resetting recovery deadline");
                for (int idle = 0; idle < 5; ++idle)
                    test::require(!host.poll(), "no busy process retries");
            }
            now += std::chrono::seconds(1);
            test::require(host.poll() && host.snapshot().state == ConnectorHostState::Failed &&
                              host.snapshot().recovery.deadline_exhausted && host.snapshot().recovery.attempts == 3 &&
                              host.snapshot().recovery.first_cause.runtime_code == 131072 &&
                              host.snapshot().recovery.cause.runtime_code == 131072 && count(1) == 0,
                          "persistent process INTERNAL exhausts bounded deadline without posts");
            ::unsetenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS");
            test::require(!host.stop(), "stop persistent internal test");
        }
        for (const auto* corruption : {"MOEX_FAKE_CALLBACK_CORRUPTION", "MOEX_FAKE_DECODE_CORRUPTION"}) {
            auto c = config_for(fixture);
            ConnectorHost host(c);
            warm(host);
            ::setenv(corruption, "1", 1);
            ::setenv("MOEX_FAKE_CALLBACK_PROCESS_INTERNAL", "1", 1);
            const auto error = host.poll();
            ::unsetenv(corruption);
            ::unsetenv("MOEX_FAKE_CALLBACK_PROCESS_INTERNAL");
            test::require(error && host.snapshot().state == ConnectorHostState::Failed &&
                              host.snapshot().recovery.attempts == 0,
                          "callback corruption remains fatal even with listener ERROR");
            test::require(host.snapshot().recovery.origin == Plaza2FailureOrigin::Callback &&
                              host.snapshot().recovery.process_cause.runtime_code == 131072 &&
                              host.snapshot().causal_health.connection == 1,
                          "explicit callback cause outranks INTERNAL process result and ERROR connection");
            test::require(!host.stop(), "stop corrupt callback test");
        }
        {
            reset();
            auto c = config_for(fixture);
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "price-revision";
            c.order.journal_root = fixture.root / "price-revision";
            ConnectorHost host(c);
            warm(host);
            const auto before = host.plan();
            test::require(before.ok && !host.begin_order(before.canonical_json, before.sha256),
                          "bind initial session terms");
            ::setenv("MOEX_FAKE_SESSION_PRICE_REVISION", "1", 1);
            const auto refused = host.submit_order();
            ::unsetenv("MOEX_FAKE_SESSION_PRICE_REVISION");
            test::require(!refused.add_submission.post_invoked && host.snapshot().publisher_calls.msgnew == 0 &&
                              host.snapshot().publisher_calls.post == 0,
                          "terms change during preflight refuses Add before allocation");
            test::require(!host.finish_order_epoch(), "definitely-not-sent epoch can close normally");
            const auto after = host.plan();
            test::require(after.ok && after.sha256 != before.sha256 &&
                              after.canonical_json.find("\"repl_rev\":4") != std::string::npos,
                          "new review binds changed committed terms");
            test::require(static_cast<bool>(host.begin_order(before.canonical_json, before.sha256)),
                          "old authority refused");
            test::require(!host.begin_order(after.canonical_json, after.sha256),
                          "new exact operator authority accepted");
            test::require(host.snapshot().publisher_calls.post == 0, "reauthorization itself posts nothing");
            test::require(static_cast<bool>(host.stop()), "authorized epoch remains protected until resolved");
        }
        for (int stage = 0; stage != 5; ++stage) {
            reset();
            ::setenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION", "1", 1);
            ::setenv("MOEX_FAKE_REGULAR_RECOVERED_ORDER", "1", 1);
            ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
            ::setenv("MOEX_FAKE_FLAT_TRADE_REPLAY", "1", 1);
            ::unsetenv("MOEX_FAKE_CANCEL_AFTER_DEL");
            auto now = std::chrono::steady_clock::now();
            auto c = config_for(fixture);
            c.transport.host.recovery_now = [&] { return now; };
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "operator-cancel-" + std::to_string(stage);
            c.order.journal_root = fixture.root / c.order.run_id;
            ConnectorHost host(c);
            warm(host);
            const auto initial = host.plan();
            test::require(initial.ok && !host.begin_order(initial.canonical_json, initial.sha256),
                          "authorize ordinary fake order");
            test::require(host.submit_order().add_submission.post_invoked, "one initial Add");
            if (stage > 0)
                test::require(host.poll_order().state == OrderLifecycleState::Working, "Working before outage");
            if (stage == 2)
                test::require(host.cancel_current_order().cancel_submission.post_invoked,
                              "cancel reply not yet read before outage");
            const auto lost_posts = count(1);
            const auto lose_and_recover = [&] {
                ::setenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS", "1", 1);
                test::require(!host.poll(), "recoverable loss");
                ::unsetenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS");
                test::require(!host.snapshot().private_streams_ready && !host.snapshot().new_order_allowed,
                              "immediate quarantine");
                now += std::chrono::seconds(1);
                for (int i = 0; i != 15 && !host.snapshot().private_streams_ready; ++i)
                    test::require(!host.poll(), "fresh bootstrap");
                test::require(host.snapshot().private_streams_ready && host.snapshot().trade_replay_complete,
                              "fresh private anchor and streams");
            };
            lose_and_recover();
            test::require(count(1) == lost_posts && host.snapshot().order_epoch_active &&
                              !host.snapshot().new_order_allowed,
                          "recovery sends no automatic Add or Cancel and preserves epoch");
            if (stage == 4) {
                ::setenv("MOEX_FAKE_FORCE_TRADE_TERMINAL", "1", 1);
                const auto terminal = host.reconcile_recovered_order();
                ::unsetenv("MOEX_FAKE_FORCE_TRADE_TERMINAL");
                test::require(terminal.outcome == RecoveredOrderOutcome::TerminalAlready && count(1) == lost_posts,
                              "positive fresh terminal proof after outage sends no Cancel");
                test::require(!host.finish_order_epoch() && !host.stop(),
                              "terminal recovery closes through normal journal");
                ::unsetenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION");
                ::unsetenv("MOEX_FAKE_REGULAR_RECOVERED_ORDER");
                ::unsetenv("MOEX_FAKE_EXT_ID");
                ::unsetenv("MOEX_FAKE_FLAT_TRADE_REPLAY");
                continue;
            }
            const auto exact = host.reconcile_recovered_order();
            test::require(exact.outcome == RecoveredOrderOutcome::ExactlyOneWorkingMatch,
                          "exact surviving regular order reconciled");
            auto path = fixture.root / (c.order.run_id + ".json");
            auto plan = host.prepare_recovered_cancel(path);
            test::require(plan.eligible() && !plan.sha256.empty(), "protected plan after fresh reconciliation");
            const auto reply_binding = [](const RecoveredCancelPlan& value) {
                const auto begin = value.canonical_json.find("\"user_id\":");
                test::require(begin != std::string::npos, "reply identity is in protected authorization");
                return value.canonical_json.substr(begin, value.canonical_json.find(',', begin) - begin);
            };
            test::require(reply_binding(plan) != "\"user_id\":" + std::to_string(c.order.cancel_user_id),
                          "recovered Cancel never reuses previous ordinary Cancel reply ID");

            (void)host.cancel_recovered_order(path, std::string(64, '0'));
            test::require(count(1) == lost_posts, "incorrect operator hash posts nothing");
            if (stage == 0) {
                lose_and_recover();
                (void)host.cancel_recovered_order(path, plan.sha256);
                test::require(count(1) == lost_posts, "generation N authority invalid in N+1");
                path = fixture.root / (c.order.run_id + "-fresh.json");
                const auto refreshed = host.prepare_recovered_cancel(path);
                test::require(refreshed.eligible() && refreshed.sha256 != plan.sha256,
                              "fresh generation needs new one-shot approval");
                test::require(reply_binding(refreshed) != reply_binding(plan),
                              "generation reauthorization reserves a new reply ID");
                plan = refreshed;
            }
            if (stage != 3)
                ::setenv("MOEX_FAKE_CANCEL_AFTER_DEL", "1", 1);
            const auto cancelled = host.cancel_recovered_order(path, plan.sha256);
            test::require(cancelled.cancel_submission.post_invoked && count(1) == lost_posts + 1,
                          "exact hash permits one DelOrder through existing publisher gate");
            test::require(std::filesystem::exists(path.string() + ".consumed") &&
                              std::filesystem::exists(path.string() + ".submission.json"),
                          "durable one-shot and publisher receipt");
            auto expected_posts = lost_posts + 1;
            if (stage == 3) {
                // The recovered Cancel itself was posted, but no reply was read.
                lose_and_recover();
                test::require(count(1) == expected_posts, "no resend of uncertain recovered Cancel");
                (void)host.cancel_recovered_order(path, plan.sha256);
                test::require(count(1) == expected_posts, "consumed old-generation approval remains unusable");
                path = fixture.root / (c.order.run_id + "-second-cancel.json");
                const auto next = host.prepare_recovered_cancel(path);
                test::require(next.eligible() && next.sha256 != plan.sha256,
                              "survivor requires a new operator approval");
                test::require(reply_binding(next) != reply_binding(plan),
                              "uncertain Cancel reply cannot correlate to a new attempt");
                plan = next;
                ::setenv("MOEX_FAKE_CANCEL_AFTER_DEL", "1", 1);
                test::require(host.cancel_recovered_order(path, plan.sha256).cancel_submission.post_invoked,
                              "second generation exact approval permits one new cancel");
                ++expected_posts;
            }
            (void)host.cancel_recovered_order(path, plan.sha256);
            test::require(count(1) == expected_posts, "consumed authority cannot repeat a command");
            for (int i = 0; i != 5 && !host.snapshot().market_safe; ++i)
                (void)host.poll_order();
            test::require(host.snapshot().market_safe && !host.finish_order_epoch(),
                          "fresh terminal evidence closes normally");
            test::require(!host.stop(), "stop resolved operator-cancel host");
            ::unsetenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION");
            ::unsetenv("MOEX_FAKE_REGULAR_RECOVERED_ORDER");
            ::unsetenv("MOEX_FAKE_EXT_ID");
            ::unsetenv("MOEX_FAKE_FLAT_TRADE_REPLAY");
            ::unsetenv("MOEX_FAKE_CANCEL_AFTER_DEL");
        }
        for (int loss_stage = 0; loss_stage != 3; ++loss_stage) {
            reset();
            ::setenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION", "1", 1);
            ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
            ::setenv("MOEX_FAKE_CANCEL_AFTER_DEL", "1", 1);
            ::setenv("MOEX_FAKE_FLAT_TRADE_REPLAY", "1", 1);
            auto now = std::chrono::steady_clock::now();
            auto c = config_for(fixture);
            c.transport.host.recovery_now = [&] { return now; };
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "loss-epoch-" + std::to_string(loss_stage);
            c.order.journal_root = fixture.root / c.order.run_id;
            ConnectorHost host(c);
            warm(host);
            const ConnectorHostOrderRequest request{.side = Plaza2TradeSide::Sell,
                                                    .price = "103000",
                                                    .base_contract_code = "RTS",
                                                    .comment = "loss-test",
                                                    .quantity = 1};
            const auto plan = host.plan_order(request);
            test::require(plan.ok && !host.begin_order(request, plan.canonical_json, plan.sha256),
                          "authorize fake interrupted epoch");
            const auto posted = host.submit_order();
            test::require(posted.add_submission.post_invoked && count(1) == 1, "one Add invoked before loss");
            if (loss_stage > 0) {
                const auto working = host.poll_order();
                test::require(working.state == OrderLifecycleState::Working, "Working before loss");
            }
            if (loss_stage == 2)
                test::require(host.cancel_current_order().cancel_submission.post_invoked, "Cancel invoked before loss");
            const auto posts = count(1);
            ::setenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS", "1", 1);
            test::require(!host.poll(), "active epoch transport loss");
            ::unsetenv("MOEX_FAKE_CONNECTION_INTERNAL_LOSS");
            (void)host.poll_order();
            test::require(host.snapshot().order_epoch_active && !host.snapshot().new_order_allowed && count(1) == posts,
                          "interrupted epoch survives with zero recovery commands");
            now += std::chrono::seconds(1);
            for (int i = 0; i < 10 && !host.snapshot().observation_ready; ++i)
                test::require(!host.poll(), "active epoch fresh observation bootstrap");
            for (int i = 0; i < 3; ++i)
                (void)host.poll_order();
            test::require(host.snapshot().observation_ready && host.snapshot().order_epoch_active &&
                              !host.snapshot().new_order_allowed && count(1) == posts,
                          "fresh observation does not abandon or retransmit an uncertain epoch");
            test::require(!host.submit_order().ok && count(1) == posts, "no Add retransmission after recovery");
            (void)host.cancel_current_order();
            test::require(count(1) == posts, "interrupted epoch cannot invoke automatic cleanup");
            (void)host.stop(); // Existing unresolved-epoch stop policy stays in force.
            ::unsetenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION");
            ::unsetenv("MOEX_FAKE_EXT_ID");
            ::unsetenv("MOEX_FAKE_CANCEL_AFTER_DEL");
            ::unsetenv("MOEX_FAKE_FLAT_TRADE_REPLAY");
        }
        {
            reset();
            auto observed_config = config_for(fixture);
            QualificationObserver observer;
            observed_config.transport.host.runtime.qualification_observer = &observer;
            observed_config.transport.host.qualification_book_observer = &observer;
            ConnectorHost host(observed_config);
            test::require(host.snapshot().state == ConnectorHostState::Created && !host.snapshot().observation_ready,
                          "created snapshot");
            test::require(static_cast<bool>(host.poll()), "poll before start refused");
            warm(host);
            test::require(static_cast<bool>(host.start()), "double start refused");
            test::require(observer.events > 0 && observer.commits > 0,
                          "qualification hooks observe real host callbacks");
            test::require(observer.forensic_rows > 0 && observer.forensic_identity && observer.forensic_equal,
                          "target payload/negotiated descriptor route agrees with generic decoder");
            const auto qualification = host.qualification_snapshot();
            test::require(qualification.aggr_online && qualification.aggr_snapshot_complete &&
                              !qualification.book.levels.empty() && !qualification.instruments.empty(),
                          "qualification samples the same committed host state");
            test::require(!qualification.limit_diagnostics.empty() &&
                              qualification.limit_diagnostics.front().private_account_code.empty(),
                          "ordinary qualification snapshot never adds raw identity bytes");
            const auto private_identity = host.qualification_snapshot(true);
            test::require(private_identity.limit_diagnostics.front().private_account_code ==
                              observed_config.order.broker_code + observed_config.order.client_code,
                          "explicit private identity query preserves exact wire code");
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
        for (const char* flag :
             {"MOEX_FAKE_NONTRADABLE_SESSION", "MOEX_FAKE_NONTRADABLE_INSTRUMENT", "MOEX_FAKE_AGGR_CROSSED",
              "MOEX_FAKE_MISSING_LIMITS", "MOEX_FAKE_CLIENT_SHAPED_UNMATCHED"}) {
            reset();
            ::setenv(flag, "1", 1);
            ConnectorHost host(config_for(fixture));
            test::require(!host.start(), "negative readiness start");
            for (int i = 0; i < 6; ++i)
                test::require(!host.poll(), "negative readiness poll");
            test::require(!host.snapshot().observation_ready && !host.plan().ok && !host.submit().ok && count(0) == 0 &&
                              count(1) == 0,
                          "readiness gate is not weakened");
            if (std::string_view(flag) == "MOEX_FAKE_CLIENT_SHAPED_UNMATCHED") {
                const auto q = host.qualification_snapshot(true);
                test::require(q.limit_diagnostics.size() == 1 &&
                                  q.limit_diagnostics.front().kind ==
                                      moex::plaza2::private_state::LimitParticipantKind::Client &&
                                  q.limit_diagnostics.front().private_account_code == "other !" &&
                                  q.matching_client_limit_rows == 0 && q.matching_broker_limit_rows == 0 &&
                                  !host.snapshot().participant_identity_exact && !host.snapshot().new_order_allowed,
                              "client-shaped unmatched T1-like row cannot authorize sending");
            }
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
            // A terminal journal releases its locks before the application
            // advances the persistent checkpoint.  Restart must trust that
            // exact, fully validated journal rather than deadlocking on the
            // stale active checkpoint when the process crashes in between.
            reset();
            ::unsetenv("MOEX_FAKE_TRADE_IDENTITY_CONFLICT");
            ::setenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION", "1", 1);
            ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
            ::setenv("MOEX_FAKE_CANCEL_AFTER_DEL", "1", 1);
            auto c = config_for(fixture);
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "terminal-before-checkpoint";
            c.order.journal_root = fixture.root / "terminal-before-checkpoint-journals";
            const auto journal_root = c.order.journal_root;
            const ConnectorHostOrderRequest request{.side = Plaza2TradeSide::Sell,
                                                    .price = "103000",
                                                    .base_contract_code = "RTS",
                                                    .comment = "terminal-crash",
                                                    .quantity = 1};
            {
                ConnectorHost host(std::move(c));
                warm(host);
                const auto plan = host.plan_order(request);
                const auto authorization = host.begin_order(request, plan.canonical_json, plan.sha256);
                test::require(plan.ok && !authorization, "terminal-before-checkpoint authorization: plan=" +
                                                             plan.message + " auth=" + authorization.message);
                test::require(!host.submit_order().ok, "terminal-before-checkpoint Add submitted");
                const auto working = host.poll_order();
                test::require(working.state == OrderLifecycleState::Working ||
                                  working.state == OrderLifecycleState::PartiallyFilled,
                              "terminal-before-checkpoint reaches a nonterminal state=" +
                                  std::string(order_lifecycle_state_name(working.state)) +
                                  " message=" + working.message);
                test::require(host.cancel_current_order().state == OrderLifecycleState::CancelPending,
                              "terminal-before-checkpoint enters CancelPending");
                OrderLifecycleResult cancelled;
                for (int attempt = 0; attempt < 4 && cancelled.state != OrderLifecycleState::Cancelled; ++attempt)
                    cancelled = host.poll_order();
                test::require(cancelled.ok && cancelled.state == OrderLifecycleState::Cancelled,
                              "terminal-before-checkpoint reaches Cancelled");
                test::require(!std::filesystem::exists(journal_root / "active" / "ext_79") &&
                                  !std::filesystem::exists(journal_root / "active" / "user_701"),
                              "terminal journal releases identifier locks before checkpoint advance");
                std::ifstream journal(journal_root / "terminal-before-checkpoint-epoch-1" / "journal.json");
                const std::string journal_text(std::istreambuf_iterator<char>(journal), {});
                test::require(journal && journal_text.find("\"finished\": true") != std::string::npos &&
                                  journal_text.find("\"market_safe_terminal\": true") != std::string::npos,
                              "terminal journal is safely complete before simulated crash");
                std::ifstream checkpoint(journal_root / "persistent_session.json");
                const std::string checkpoint_text(std::istreambuf_iterator<char>(checkpoint), {});
                test::require(checkpoint &&
                                  checkpoint_text.find("\"phase\": \"add_may_have_been_sent\"") != std::string::npos,
                              "crash leaves the persistent checkpoint active");
                // Deliberately do not call finish_order_epoch().
            }
            auto restart_config = config_for(fixture);
            restart_config.purpose = HostPurpose::OrderTest;
            restart_config.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            restart_config.transport.host.arm_state.test_order_send_armed = true;
            restart_config.order.run_id = "terminal-before-checkpoint";
            restart_config.order.journal_root = journal_root;
            ConnectorHost restarted(std::move(restart_config));
            test::require(restarted.snapshot().order_epoch_active && !restarted.snapshot().new_order_allowed,
                          "safe terminal restart initially restores the active checkpoint");
            // Historical terminal recovery must not be coupled to the
            // current market book.  Make the current AGGR20 deliberately
            // crossed before restart; reconciliation still needs only the
            // private replication and anchored TRADE surfaces.
            ::setenv("MOEX_FAKE_AGGR_CROSSED", "1", 1);
            test::require(!restarted.start(), "safe terminal crossed-book restart starts");
            for (unsigned i = 0;
                 i < 10 && !(restarted.snapshot().private_streams_ready && restarted.snapshot().trade_replay_complete);
                 ++i) {
                test::require(!restarted.poll(), "safe terminal crossed-book restart poll");
            }
            test::require(restarted.snapshot().private_streams_ready && restarted.snapshot().trade_replay_complete,
                          "safe terminal crossed-book restart has current private replication");
            const auto msgnew_before_reconciliation = count(0);
            const auto post_before_reconciliation = count(1);
            const auto reconciliation = restarted.reconcile();
            test::require(reconciliation.ok && reconciliation.run_found && reconciliation.resolved &&
                              !reconciliation.locks_retained &&
                              reconciliation.state == OrderLifecycleState::Cancelled &&
                              !restarted.snapshot().order_epoch_active && !restarted.snapshot().new_order_allowed &&
                              count(0) == msgnew_before_reconciliation && count(1) == post_before_reconciliation,
                          "safe terminal no-lock journal resolves despite crossed AGGR20 without publisher calls: " +
                              reconciliation.message + " ok=" + std::to_string(reconciliation.ok) +
                              " found=" + std::to_string(reconciliation.run_found) +
                              " resolved=" + std::to_string(reconciliation.resolved) +
                              " locks=" + std::to_string(reconciliation.locks_retained) +
                              " state=" + std::string(order_lifecycle_state_name(reconciliation.state)) +
                              " active=" + std::to_string(restarted.snapshot().order_epoch_active) +
                              " allowed=" + std::to_string(restarted.snapshot().new_order_allowed) +
                              " msgnew=" + std::to_string(count(0)) + " post=" + std::to_string(count(1)));
            std::ifstream idle_checkpoint(journal_root / "persistent_session.json");
            const std::string idle_text(std::istreambuf_iterator<char>(idle_checkpoint), {});
            test::require(idle_checkpoint && idle_text.find("\"phase\": \"idle\"") != std::string::npos,
                          "safe terminal reconciliation advances the persistent checkpoint to idle");
            test::require(!restarted.stop(), "safe terminal crossed-book restart stops after reconciliation");
            ::unsetenv("MOEX_FAKE_AGGR_CROSSED");
            auto restored_config = config_for(fixture);
            restored_config.purpose = HostPurpose::OrderTest;
            restored_config.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            restored_config.transport.host.arm_state.test_order_send_armed = true;
            restored_config.order.run_id = "terminal-before-checkpoint";
            restored_config.order.journal_root = journal_root;
            ConnectorHost restored(std::move(restored_config));
            warm(restored);
            test::require(restored.snapshot().new_order_allowed,
                          "safe terminal restart re-enables a new epoch after AGGR20 is restored");
            const auto next_plan = restored.plan_order(request);
            test::require(next_plan.ok && next_plan.canonical_json.find("\"ext_id\": 80") != std::string::npos,
                          "safe terminal reconciliation advances to epoch-2 identifiers");
            test::require(!restored.stop(), "safe terminal restored host stops after reconciliation");
            ::unsetenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION");
            ::unsetenv("MOEX_FAKE_EXT_ID");
            ::unsetenv("MOEX_FAKE_PUB_REPLY_ORDER_ID");
            ::unsetenv("MOEX_FAKE_CANCEL_AFTER_DEL");
            ::unsetenv("MOEX_FAKE_AGGR_CROSSED");
            ::unsetenv("MOEX_FAKE_TRADE_IDENTITY_CONFLICT");
        }
        {
            // Missing locks alone are never proof of safety: a corrupted
            // terminal marker keeps the recovered checkpoint blocked.
            reset();
            ::unsetenv("MOEX_FAKE_TRADE_IDENTITY_CONFLICT");
            ::setenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION", "1", 1);
            ::setenv("MOEX_FAKE_EXT_ID", "79", 1);
            ::setenv("MOEX_FAKE_PUB_REPLY_ORDER_ID", "20003", 1);
            ::setenv("MOEX_FAKE_CANCEL_AFTER_DEL", "1", 1);
            auto c = config_for(fixture);
            c.purpose = HostPurpose::OrderTest;
            c.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            c.transport.host.arm_state.test_order_send_armed = true;
            c.order.run_id = "corrupt-terminal-journal";
            c.order.journal_root = fixture.root / "corrupt-terminal-journal-journals";
            const auto journal_root = c.order.journal_root;
            const ConnectorHostOrderRequest request{.side = Plaza2TradeSide::Sell,
                                                    .price = "103000",
                                                    .base_contract_code = "RTS",
                                                    .comment = "corrupt-journal",
                                                    .quantity = 1};
            const auto journal_path = journal_root / "corrupt-terminal-journal-epoch-1" / "journal.json";
            {
                ConnectorHost host(std::move(c));
                warm(host);
                const auto plan = host.plan_order(request);
                test::require(plan.ok && !host.begin_order(request, plan.canonical_json, plan.sha256),
                              "corrupt-terminal journal authorization");
                test::require(!host.submit_order().ok, "corrupt-terminal journal Add submitted");
                const auto working = host.poll_order();
                test::require(working.state == OrderLifecycleState::Working ||
                                  working.state == OrderLifecycleState::PartiallyFilled,
                              "corrupt-terminal journal reaches a nonterminal state=" +
                                  std::string(order_lifecycle_state_name(working.state)) +
                                  " message=" + working.message);
                test::require(host.cancel_current_order().state == OrderLifecycleState::CancelPending,
                              "corrupt-terminal journal enters CancelPending");
                OrderLifecycleResult cancelled;
                for (int attempt = 0; attempt < 4 && cancelled.state != OrderLifecycleState::Cancelled; ++attempt)
                    cancelled = host.poll_order();
                test::require(cancelled.ok && cancelled.state == OrderLifecycleState::Cancelled,
                              "corrupt-terminal journal reaches Cancelled");
                std::ifstream input(journal_path);
                const std::string original(std::istreambuf_iterator<char>(input), {});
                test::require(static_cast<bool>(input), "corrupt-terminal journal can be read");
                const auto marker = original.find("\"finished\": true");
                test::require(marker != std::string::npos, "corrupt-terminal journal has finished marker");
                auto corrupted = original;
                corrupted.replace(marker, std::string("\"finished\": true").size(), "\"finished\": false");
                test::write_text_file(journal_path, corrupted);
                test::require(!std::filesystem::exists(journal_root / "active" / "ext_79"),
                              "corrupt-terminal journal still has no identifier locks");
                // Deliberately do not call finish_order_epoch().
            }
            auto restart_config = config_for(fixture);
            restart_config.purpose = HostPurpose::OrderTest;
            restart_config.transport.host.mode = Plaza2TestSessionHostMode::LiveTestAuthorizedSend;
            restart_config.transport.host.arm_state.test_order_send_armed = true;
            restart_config.order.run_id = "corrupt-terminal-journal";
            restart_config.order.journal_root = journal_root;
            ConnectorHost restarted(std::move(restart_config));
            warm(restarted);
            const auto msgnew_before_reconciliation = count(0);
            const auto post_before_reconciliation = count(1);
            const auto unresolved = restarted.reconcile();
            test::require(!unresolved.ok && unresolved.run_found && !unresolved.resolved &&
                              !unresolved.locks_retained && restarted.snapshot().order_epoch_active &&
                              !restarted.snapshot().new_order_allowed && count(0) == msgnew_before_reconciliation &&
                              count(1) == post_before_reconciliation,
                          "corrupt no-lock journal remains unresolved and blocks a new Add: " + unresolved.message);
            std::ifstream payload_input(journal_path);
            const std::string corrupted(std::istreambuf_iterator<char>(payload_input), {});
            test::require(static_cast<bool>(payload_input), "corrupt-terminal journal remains readable");
            const auto payload_marker = corrupted.find("\"payload_sha256\": \"");
            test::require(payload_marker != std::string::npos, "corrupt-terminal journal has payload hash");
            const auto payload_begin = payload_marker + std::string("\"payload_sha256\": \"").size();
            auto corrupted_payload = corrupted;
            corrupted_payload.replace(payload_begin, 64, std::string(64, '0'));
            test::write_text_file(journal_path, corrupted_payload);
            const auto payload_unresolved = restarted.reconcile();
            test::require(!payload_unresolved.ok && payload_unresolved.run_found && !payload_unresolved.resolved &&
                              !payload_unresolved.locks_retained && restarted.snapshot().order_epoch_active &&
                              !restarted.snapshot().new_order_allowed && count(0) == msgnew_before_reconciliation &&
                              count(1) == post_before_reconciliation,
                          "corrupt payload hash cannot resolve a no-lock journal: " + payload_unresolved.message);
            // The unresolved recovered epoch intentionally prevents stop.
            ::unsetenv("MOEX_FAKE_PERSISTENT_ORDER_SESSION");
            ::unsetenv("MOEX_FAKE_EXT_ID");
            ::unsetenv("MOEX_FAKE_PUB_REPLY_ORDER_ID");
            ::unsetenv("MOEX_FAKE_CANCEL_AFTER_DEL");
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
