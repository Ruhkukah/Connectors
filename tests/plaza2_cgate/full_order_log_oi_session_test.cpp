#include "moex/plaza2_trade/cgate_session.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_oi.hpp"
#include "moex/plaza2/cgate/plaza2_replication_tables.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include <array>
#include <iostream>
using namespace moex::plaza2;
using namespace moex::plaza2_trade;
namespace f = test::fake;
int main(int argc, char** argv) {
    try {
        test::require(argc == 2, "fake runtime argument");
        const auto root = test::make_temp_directory("fol_oi_session");
        const auto fixture =
            test::materialize_runtime_fixture(root, argv[1], cgate::Plaza2Environment::Test,
                                              test::build_vendor_like_runtime_scheme("SPECTRA 9.12.0", "9.12", "T1"));
        f::Control fake(fixture.library_path);
        const std::array<std::int32_t, 1> ids{1001};
        cgate::Plaza2FullOrderLog book(ids, 1024, 256);
        cgate::FullOrderLogOpenInterest oi(ids);
        CgateSessionConfig c;
        c.runtime.runtime_root = fixture.root;
        c.runtime.scheme_dir = fixture.scheme_dir;
        c.runtime.env_open_settings = "ini=config/t1.ini;key=00000000";
        c.connection_settings = "p2tcp://127.0.0.1:4102;app_name=offline_oi_test";
        c.read_only_market_data = true;
        c.process_timeout_ms = 0;
        c.market_data_isin_ids = {1001};
        c.private_streams = {{generated::StreamCode::kFortsRefdataRepl,
                              cgate::required_replication_listener_settings(generated::StreamCode::kFortsRefdataRepl),
                              "mode=snapshot+online"}};
        c.full_order_log_stream = {cgate::kFullOrderLogStreamCode,
                                   cgate::full_order_log_listener_settings(fixture.scheme_dir), ""};
        c.full_order_log_handler = &book;
        c.full_order_log_oi_handler = &oi;
        f::Scenario scenario;
        scenario.spectra912_refdata_schema = true;
        scenario.options[static_cast<std::size_t>(f::Option::FullOrderLogRefdata)] = "1";
        scenario.options[static_cast<std::size_t>(f::Option::DisablePublisher)] = "1";
        scenario.options[static_cast<std::size_t>(f::Option::DisableReplyListener)] = "1";
        fake.configure(scenario);
        {
            auto bad = c;
            bad.full_order_log_handler = nullptr;
            bad.full_order_log_stream = {};
            CgateSession reject(bad);
            test::require(reject.start().code == cgate::Plaza2ErrorCode::InvalidConfiguration &&
                              fake.opens(generated::StreamCode::kFortsDealsRepl) == 0,
                          "OI cannot leak into non-FOL profile");
        }
        {
            CgateSession s(c);
            test::require(!s.start(), "FOL OI start");
            for (int i = 0; i < 12; ++i)
                test::require(!s.poll(false), "FOL OI poll");
            test::require(book.valid() && oi.online() && oi.value(1001) && oi.value(1001)->quantity == 123,
                          "native adapter routes committed raw OI");
            test::require(fake.listener_settings(generated::StreamCode::kFortsDealsRepl) ==
                              "p2repl://FORTS_DEALS_REPL;tables=deal",
                          "exact OI one-table subscription");
            test::require(!s.stop(), "OI stop");
        }
        fake.configure(scenario);
        const auto old_deals_opens = fake.opens(generated::StreamCode::kFortsDealsRepl);
        c.full_order_log_oi_handler = nullptr;
        {
            CgateSession s(c);
            test::require(!s.start(), "legacy FOL start");
            for (int i = 0; i < 12; ++i)
                test::require(!s.poll(false), "legacy FOL poll");
            test::require(fake.opens(generated::StreamCode::kFortsDealsRepl) == old_deals_opens,
                          "null OI hook preserves old listener inventory");
            test::require(!s.stop(), "legacy FOL stop");
        }
        test::remove_tree(root);
        std::cout << "FOL-only OI routing and null-handler isolation passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
