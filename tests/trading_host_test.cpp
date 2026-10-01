#include "moex/connector_host/operator_config.hpp"
#include "moex/connector_host/trading_host.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

namespace cg = moex::plaza2::cgate;
namespace test = moex::plaza2::test;
using namespace moex::connector_host;
int main(int argc, char** argv) {
    try {
        test::require(argc == 2, "fake runtime path required");
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
