#include "moex/connector_host/operator_config.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include "scope_exit.hpp"
#include "private_delta_host_regression.hpp"
#include "late_move_host_regression.hpp"
#include "move_link_host_regression.hpp"
#include "add_reply_host_regression.hpp"
#include "lost_add_rebuild_host_regression.hpp"
#include "add_conflict_host_regression.hpp"
#include "immediate_dispatch_host_regression.hpp"
#include "storage_guard_regression.hpp"
#include "storage_recovery_regression.hpp"
#include "storage_halt_cancel_regression.hpp"
#include "exchange_message_regression.hpp"
#include "transport_status_regression.hpp"
#include "replication_journal_regression.hpp"
#include "reload_missing_host_regression.hpp"
#include "inflight_rebuild_host_regression.hpp"
#include "opening_auction_host_regression.hpp"
#include "spread_scope_host_regression.hpp"
#include "risk_limits_host_regression.hpp"
#include "userbook_barrier_host_regression.hpp"
#include "multileg_history_host_regression.hpp"
#include "userbook_replacement_host_regression.hpp"
#include "instance_cancel_host_regression.hpp"
#include "plaza2_cgate/field_read_contract.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <sstream>

namespace cg = moex::plaza2::cgate;
namespace test = moex::plaza2::test;
using namespace moex::connector_host;
namespace {
void scope_exit_regression() {
    struct Host {
        int& calls;
        cg::Plaza2Error stop() {
            ++calls;
            return {.code = cg::Plaza2ErrorCode::RuntimeCallFailed, .message = "mock stop failure"};
        }
    };
    int calls{};
    Host host{calls};
    const auto early_exit = [&] {
        ScopeExit guard([&] { (void)host.stop(); });
        return 3;
    };
    test::require(early_exit() == 3 && calls == 1, "early host exit did not stop exactly once");
    try {
        ScopeExit guard([&] { (void)host.stop(); });
        throw std::runtime_error("poll exception");
    } catch (const std::runtime_error&) {
    }
    test::require(calls == 2, "exception unwinding did not stop the host");
    {
        ScopeExit guard([&] { (void)host.stop(); });
        const auto error = host.stop();
        guard.release();
        test::require(error.code == cg::Plaza2ErrorCode::RuntimeCallFailed, "explicit stop discarded its error result");
    }
    test::require(calls == 3, "explicit stop and destruction called stop twice");
    bool reported{};
    try {
        ScopeExit report([&] { reported = true; });
        ScopeExit shutdown([&] {
            ++calls;
            throw std::runtime_error("stop exception");
        });
        throw std::runtime_error("poll exception");
    } catch (const std::runtime_error&) {
    }
    test::require(calls == 4 && reported, "shutdown exception prevented outstanding-order reporting");
}
void named_journal_regression(TradingHostConfig config, const test::fake::Control& control,
                              const std::filesystem::path& root) {
    test::fake::Scenario scenario{.suppress_initial_orders = true, .zero_position = true, .client_code = "BRK1C01"};
    control.configure(scenario);
    config.journal_path = root / "named-journal.ndjson";
    config.identity_state_path = root / "named-journal.state";
    config.source_git_sha = std::string(40, 'a');
    config.binary_sha256 = std::string(64, 'b');
    config.session.connection_settings += ";password=crt6-private-password";
    auto now = OrderManager::Clock::time_point{};
    config.session.recovery_now = [&] { return now; };
    CgateTradingHost host(config);
    test::require(!host.start(), "named journal fixture start");
    for (int i = 0; i < 30; ++i)
        test::require(!host.poll(), "named journal bootstrap");
    const std::string refused_line = "place refused 1001 buy 0 103000 day";
    host.record_operator_input(refused_line, "command_socket");
    const auto refusal = host.place({.client_order_id = "refused", .isin_id = 1001, .price = "103000", .quantity = 0});
    test::require(!refusal.empty(), "local refusal fixture admitted quantity zero");
    host.record_local_refusal(refused_line, refusal, "command_socket");
    control.set(test::fake::Option::PublisherClosed, "1");
    for (int i = 0; i < 3; ++i)
        test::require(!host.poll(), "publisher loss while connection ACTIVE");
    control.clear(test::fake::Option::PublisherClosed);
    now += std::chrono::seconds(2);
    for (int i = 0; i < 30; ++i)
        test::require(!host.poll(), "publisher recovery while connection ACTIVE");
    control.set(test::fake::Option::ConnToOpening, "1");
    for (int i = 0; i < 3; ++i)
        test::require(!host.poll(), "connection loss journal");
    control.clear(test::fake::Option::ConnToOpening);
    now += std::chrono::seconds(2);
    for (int i = 0; i < 30; ++i)
        test::require(!host.poll(), "connection recovery journal");
    test::require(!host.stop(), "named journal fixture stop");
    std::ifstream journal(config.journal_path);
    std::string line;
    bool startup{}, named_row{}, publisher_loss{};
    std::size_t lost{}, restored{}, inputs{}, refusals{};
    while (std::getline(journal, line)) {
        if (line.find("\"event\":\"startup\"") != std::string::npos) {
            startup = line.find("\"source_git_sha\":") != std::string::npos &&
                      line.find(std::string(40, 'a')) != std::string::npos &&
                      line.find("\"binary_sha256\":") != std::string::npos &&
                      line.find(std::string(64, 'b')) != std::string::npos &&
                      line.find("\"rate\":30") != std::string::npos && line.find("\"risk\":") != std::string::npos &&
                      line.find("p2tcp://127.0.0.1:4101;app_name=moex_connector;") != std::string::npos &&
                      line.find("p2repl://FORTS_TRADE_REPL") != std::string::npos &&
                      line.find("p2mq://FORTS_SRV") != std::string::npos;
            test::require(line.find("crt6-private-password") == std::string::npos &&
                              line.find("key=00000000") == std::string::npos,
                          "startup journal exposed a CGate setting secret");
        }
        named_row |= line.find("\"stream\":\"FORTS_TRADE_REPL\"") != std::string::npos &&
                     line.find("\"table\":\"heartbeat\"") != std::string::npos;
        if (line.find("\"event\":\"link_lost\"") != std::string::npos) {
            ++lost;
            publisher_loss |= line.find("\"connection_state\":3,\"publisher_state\":0") != std::string::npos;
        }
        restored += line.find("\"event\":\"link_restored\"") != std::string::npos;
        inputs += line.find("\"event\":\"operator_input\"") != std::string::npos &&
                  line.find(refused_line) != std::string::npos;
        refusals += line.find("\"event\":\"local_refusal\"") != std::string::npos &&
                    line.find(refused_line) != std::string::npos;
    }
    test::require(startup && named_row, "journal omitted masked startup configuration or named replication streams");
    test::require(lost == 2 && restored == 2 && publisher_loss,
                  "journal omitted/doubled connection or ACTIVE-connection publisher loss/restoration");
    test::require(inputs == 1 && refusals == 1, "journal lost the operator input or its local refusal");
}
} // namespace
int main(int argc, char** argv) {
    try {
        moex::plaza2::test::FieldReadContract field_reads;
        test::require(argc == 2, "fake runtime path required");
        scope_exit_regression();
        const auto root = test::make_temp_directory("trading-host");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cg::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA9.9.0", "9.9", "T1"));
        moex::plaza2::test::fake::Control fake(fixture.library_path);
        ::setenv("MOEX_PLAZA2_TEST_CREDENTIALS", "fake-test-only", 1);
        ::setenv("MOEX_PLAZA2_CGATE_SOFTWARE_KEY", "00000000", 1);
        fake.set(moex::plaza2::test::fake::Option::ClientCode, "BRK1C01");
        fake.set(moex::plaza2::test::fake::Option::AggrWrongSession, "1");
        fake.set(moex::plaza2::test::fake::Option::DelayUserorderbook, "1");
        fake.set(moex::plaza2::test::fake::Option::UserbookOnlyOrder, "1");
        Plaza2HostConfigInputs input;
        input.runtime_root = fixture.root;
        input.library_path = fixture.library_path;
        input.scheme_dir = fixture.scheme_dir;
        input.config_dir = fixture.config_dir;
        input.env_open_settings = "ini=config/t1.ini;key=00000000";
        input.credentials_env_var = "MOEX_PLAZA2_TEST_CREDENTIALS";
        input.software_key_env_var = "MOEX_PLAZA2_CGATE_SOFTWARE_KEY";
        input.broker_code = "BRK1";
        input.client_code = "C01";
        input.isin_ids = {1001};
        input.allow_orders = true;
        TradingHostConfig config;
        config.session = build_plaza2_host_config(input).transport.host;
        config.session.mode = moex::plaza2_trade::CgateSessionMode::OfflineFake;
        config.session.process_timeout_ms = 0;
        config.orders.broker_code = input.broker_code;
        config.orders.client_code = input.client_code;
        config.isin_ids = {1001};
        config.journal_path = root / "events.ndjson";
        moex::connector_host::regression::explicit_test_risk(config);
        moex::connector_host::regression::instance_cancel_host_regression(config, fake, root);
        moex::connector_host::inflight_rebuild_host_regression(config, fake, root);
        moex::connector_host::regression::storage_recovery_regression(config, fake, root);
        moex::connector_host::regression::multileg_history_host_regression(config, fake, root);
        moex::connector_host::regression::userbook_replacement_host_regression(config, fake, root);
        moex::connector_host::userbook_barrier_host_regression(config, fake, root);
        moex::connector_host::regression::exchange_message_regression(config, fake, root);
        moex::connector_host::regression::transport_status_regression(config, fake, root);
        moex::connector_host::regression::replication_journal_regression(config, fake, root);
        moex::connector_host::regression::storage_halt_cancel_regression(config, fake, root);
        moex::connector_host::regression::risk_limits_host_regression(config, fake, root);
        moex::connector_host::reload_missing_host_regression(config, fake, root);
        moex::connector_host::opening_auction_host_regression(config, fake, root);
        moex::connector_host::spread_scope_host_regression(config, fake, root);
        moex::connector_host::regression::immediate_dispatch_host_regression(config, fake, root);
        moex::connector_host::regression::storage_capacity_guard(config, fake, root, false);
        moex::connector_host::regression::storage_capacity_guard(config, fake, root, true);
        moex::connector_host::regression::storage_durable_cancel_guard(config, fake, root);
        named_journal_regression(config, fake, root);
        fake.configure(test::fake::Scenario{.client_code = "BRK1C01"});
        fake.set(moex::plaza2::test::fake::Option::AggrWrongSession, "1");
        fake.set(moex::plaza2::test::fake::Option::DelayUserorderbook, "1");
        fake.set(moex::plaza2::test::fake::Option::UserbookOnlyOrder, "1");
        {
            CgateTradingHost host(config);
            test::require(!host.start(), "production owner starts with fake CGate");
            for (int i = 0; i < 30; ++i)
                test::require(!host.poll(), "production owner bootstrap");
            test::require(
                !host.place({.client_order_id = "premature", .isin_id = 1001, .price = "103000", .quantity = 2})
                     .empty(),
                "new Add bypassed delayed startup exposure reconstruction");
            fake.clear(moex::plaza2::test::fake::Option::DelayUserorderbook);
            for (int i = 0; i < 15; ++i)
                test::require(!host.poll(), "delayed USERORDERBOOK completion");
            test::require(host.status().find("recovered:321:20009") != std::string::npos,
                          "late USERORDERBOOK-only working order was not reconstructed");
            fake.set(moex::plaza2::test::fake::Option::PubReplyOrderId, "61001");
            test::require(
                host.place({.client_order_id = "first", .isin_id = 1001, .price = "103000", .quantity = 2}).empty(),
                "production owner did not allow quantity2 while AGGR invalid");
            for (int i = 0; i < 3; ++i)
                test::require(!host.poll(), "production Add dispatch/reply");
            test::require(host.status().find("61001") != std::string::npos,
                          "179 reply did not reach production manager");
            fake.set(moex::plaza2::test::fake::Option::PubReplyOrderId, "61002");
            test::require(
                host.place({.client_order_id = "second", .isin_id = 1001, .price = "103000", .quantity = 3}).empty(),
                "production owner refused concurrent order");
            for (int i = 0; i < 3; ++i)
                test::require(!host.poll(), "second concurrent Add dispatch/reply");
            test::require(host.status().find("61002") != std::string::npos, "second reply correlation lost");
            test::require(host.move("second", "103250", 2).empty(), "production Move refused");
            for (int i = 0; i < 3; ++i)
                test::require(!host.poll(), "Move dispatch/reply");
            test::require(host.status().find("\"order_id\":20004") != std::string::npos,
                          "176 Move reply did not reach production manager");
            test::require(host.cancel("first").empty(), "production cancel refused");
            for (int i = 0; i < 3; ++i)
                test::require(!host.poll(), "cancel dispatch/reply");
            bool wrong_thread{};
            std::thread other([&] {
                try {
                    const auto unused = host.status();
                    (void)unused;
                } catch (const std::logic_error&) {
                    wrong_thread = true;
                }
            });
            other.join();
            test::require(wrong_thread, "snapshot queried CGate across threads");
            test::require(!host.stop(), "production host stops");
            const auto runtime = ::dlopen(fixture.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
            test::require(runtime != nullptr, "load fake environment-open counter");
            const auto environment_opens =
                reinterpret_cast<std::uint64_t (*)()>(::dlsym(runtime, "moex_fake_environment_open_count"));
            test::require(environment_opens != nullptr, "fake environment-open counter missing");
            const auto before_restart = environment_opens();
            const auto restart_error = host.start();
            const auto after_restart = environment_opens();
            ::dlclose(runtime);
            test::require(restart_error.code == cg::Plaza2ErrorCode::InvalidConfiguration &&
                              after_restart == before_restart,
                          "stopped trading host reopened CGate without restoring shutdown ownership");
        }
        std::ifstream journal(config.journal_path);
        std::string line;
        bool command{}, reply{}, market{}, sys_message{};
        while (std::getline(journal, line)) {
            command |= line.find("\"event\":\"command\"") != std::string::npos;
            reply |= line.find("\"event\":\"reply\"") != std::string::npos;
            market |= line.find("\"event\":\"stream_row\"") != std::string::npos;
            sys_message |= line.find("\"name\":\"sys_messages\"") != std::string::npos;
            test::require(line.find("\"utc\":") != std::string::npos && line.find("\"msk\":") != std::string::npos,
                          "interaction missing UTC/MSK timestamp");
        }
        test::require(command && reply && market && sys_message,
                      "interaction log omitted command/reply/market/exchange message");
        {
            test::fake::Scenario scenario;
            scenario.client_code = "BRK1C01";
            scenario.suppress_initial_orders = true;
            fake.configure(scenario);
            auto broken_storage = config;
            broken_storage.journal_path = root / "storage-failure.ndjson";
            broken_storage.identity_state_path = root / "storage-failure.state";
            CgateTradingHost host(broken_storage);
            std::ostringstream diagnostics;
            ScopeExit shutdown([&] { (void)host.stop(); });
            test::require(!host.start(), "storage-failure host start");
            for (int i = 0; i < 30; ++i)
                test::require(!host.poll(), "storage-failure host bootstrap");
            std::ostringstream flat_diagnostics;
            host.report_outstanding_orders(flat_diagnostics);
            test::require(flat_diagnostics.str().empty(), "flat host printed a working-order shutdown warning");
            fake.set(test::fake::Option::PubReplyOrderId, "62001");
            test::require(host.place({.client_order_id = "working-before-storage-failure",
                                      .isin_id = 1001,
                                      .price = "103000",
                                      .quantity = 2})
                              .empty(),
                          "storage-failure working Add refused");
            for (int i = 0; i < 3; ++i)
                test::require(!host.poll(), "storage-failure working Add dispatch/reply");
            test::require(host.status().find("\"order_id\":62001") != std::string::npos,
                          "storage-failure fixture lacks an established working order");
            const auto posts = fake.commands().size();
            test::require(std::filesystem::remove(broken_storage.identity_state_path), "remove identity checkpoint");
            std::filesystem::create_directory(broken_storage.identity_state_path);
            // A time-based checkpoint failure blocks entry while the native
            // owner remains available for reconciliation and durable-ID cancels.
            std::this_thread::sleep_for(std::chrono::milliseconds(270));
            const auto poll_error = host.poll();
            test::require(!poll_error && fake.commands().size() == posts &&
                              host.status().find("\"cancel_only\":true") != std::string::npos,
                          "journal checkpoint failure stopped the native owner instead of entering cancel-only mode");
            test::require(!host.place({.client_order_id = "blocked-by-storage-failure",
                                       .isin_id = 1001,
                                       .price = "103000",
                                       .quantity = 2})
                                  .empty() &&
                              fake.commands().size() == posts,
                          "storage-failed host admitted another Add");
            test::require(!host.move("working-before-storage-failure", "103250", 2).empty(),
                          "storage-failed host admitted a Move");
            test::require(host.cancel("working-before-storage-failure").empty() &&
                              fake.commands().size() == posts + 1 && fake.commands().back().name == "DelOrder",
                          "storage-failed owner did not post a cancellation from its durable ID block");
            const auto native_polls = fake.process_count();
            test::require(!host.poll() && fake.process_count() > native_polls,
                          "storage-failed owner stopped native replication/reply processing");
            cg::Plaza2Error stop_error;
            const auto closes_before = fake.successful_closes();
            try {
                ScopeExit report([&] { host.report_outstanding_orders(diagnostics); });
                stop_error = host.stop();
                shutdown.release();
            } catch (...) {
                throw std::runtime_error("storage failure escaped stop before CGate cleanup");
            }
            test::require(stop_error.code == cg::Plaza2ErrorCode::RuntimeCallFailed,
                          "shutdown discarded its storage failure");
            test::require(diagnostics.str().find("Working orders may remain on the exchange") != std::string::npos &&
                              diagnostics.str().find("order_id=62001") != std::string::npos,
                          "fatal shutdown omitted its warning and known working order");
            const auto closes_after = fake.successful_closes();
            for (std::size_t object = 0; object < closes_before.size(); ++object)
                test::require(closes_after[object] > closes_before[object],
                              "failed journal prevented native environment/connection/listener/publisher closure");
            test::require(host.stop().code == stop_error.code, "repeated shutdown lost its stored result");
            test::require(fake.commands().size() == posts + 1, "shutdown sent an unrequested command");
        }
        moex::connector_host::private_delta_host_regression(config, fake, root);
        moex::connector_host::late_move_host_regression(config, fake, root);
        moex::connector_host::move_link_host_regression(config, fake, root);
        moex::connector_host::add_reply_host_regression(config, fake, root);
        moex::connector_host::lost_add_rebuild_host_regression(config, fake, root);
        moex::connector_host::add_conflict_host_regression(config, fake, root);
        fake.clear(moex::plaza2::test::fake::Option::AggrWrongSession);
        fake.clear(moex::plaza2::test::fake::Option::UserbookOnlyOrder);
        fake.clear(moex::plaza2::test::fake::Option::PubReplyOrderId);
        test::remove_tree(root);
        field_reads.verify("native trading and journal", 170);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
