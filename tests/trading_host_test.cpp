#include "moex/connector_host/operator_config.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include "host_stop_guard.hpp"

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
void stop_guard_regression() {
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
        HostStopGuard guard(host);
        return 3;
    };
    test::require(early_exit() == 3 && calls == 1, "early host exit did not stop exactly once");
    try {
        HostStopGuard guard(host);
        throw std::runtime_error("poll exception");
    } catch (const std::runtime_error&) {
    }
    test::require(calls == 2, "exception unwinding did not stop the host");
    {
        HostStopGuard guard(host);
        test::require(guard.stop().code == cg::Plaza2ErrorCode::RuntimeCallFailed && guard.stop(),
                      "explicit stop discarded its error result");
    }
    test::require(calls == 3, "explicit stop and destruction called stop twice");
}
} // namespace
int main(int argc, char** argv) {
    try {
        test::require(argc == 2, "fake runtime path required");
        stop_guard_regression();
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
            test::require(
                host.place({.client_order_id = "first", .isin_id = 1001, .price = "103000", .quantity = 2}).empty(),
                "production owner did not allow quantity2 while AGGR invalid");
            fake.set(moex::plaza2::test::fake::Option::PubReplyOrderId, "61001");
            for (int i = 0; i < 3; ++i)
                test::require(!host.poll(), "production Add dispatch/reply");
            test::require(host.status().find("61001") != std::string::npos,
                          "179 reply did not reach production manager");
            test::require(
                host.place({.client_order_id = "second", .isin_id = 1001, .price = "103000", .quantity = 3}).empty(),
                "production owner refused concurrent order");
            fake.set(moex::plaza2::test::fake::Option::PubReplyOrderId, "61002");
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
            HostStopGuard guard(host, [&] { host.report_outstanding_orders(diagnostics); });
            test::require(!host.start(), "storage-failure host start");
            for (int i = 0; i < 30; ++i)
                test::require(!host.poll(), "storage-failure host bootstrap");
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
            test::require(host.place({.client_order_id = "blocked-by-storage-failure",
                                      .isin_id = 1001,
                                      .price = "103000",
                                      .quantity = 2})
                              .empty(),
                          "queue Add before reservation checkpoint fails");
            const auto poll_error = host.poll();
            test::require(poll_error.code == cg::Plaza2ErrorCode::RuntimeCallFailed && fake.commands().size() == posts,
                          "journal checkpoint failure did not stop sends before dispatch");
            cg::Plaza2Error stop_error;
            try {
                stop_error = guard.stop();
            } catch (...) {
                throw std::runtime_error("storage failure escaped stop before CGate cleanup");
            }
            test::require(stop_error.code == cg::Plaza2ErrorCode::RuntimeCallFailed,
                          "shutdown discarded its storage failure");
            test::require(diagnostics.str().find("Working orders may remain on the exchange") != std::string::npos &&
                              diagnostics.str().find("order_id=62001") != std::string::npos,
                          "fatal shutdown omitted its warning and known working order");
            std::ifstream closed_log(broken_storage.journal_path);
            std::string closed_line;
            bool connection_closed{}, publisher_closed{}, listener_closed{};
            while (std::getline(closed_log, closed_line)) {
                if (closed_line.find("\"operation\":\"close\"") == std::string::npos)
                    continue;
                connection_closed |= closed_line.find("\"object\":\"connection\"") != std::string::npos;
                publisher_closed |= closed_line.find("\"object\":\"publisher\"") != std::string::npos;
                listener_closed |= closed_line.find("\"object\":\"listener\"") != std::string::npos;
            }
            test::require(connection_closed && publisher_closed && listener_closed,
                          "journal failure prevented CGate objects from closing");
            test::require(host.stop().code == stop_error.code, "repeated shutdown lost its stored result");
            test::require(fake.commands().size() == posts, "shutdown bypassed durability guard to send commands");
        }
        fake.clear(moex::plaza2::test::fake::Option::AggrWrongSession);
        fake.clear(moex::plaza2::test::fake::Option::UserbookOnlyOrder);
        fake.clear(moex::plaza2::test::fake::Option::PubReplyOrderId);
        test::remove_tree(root);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
