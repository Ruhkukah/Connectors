#include "moex/plaza2_trade/cgate_session.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_oi.hpp"
#include "moex/plaza2/cgate/plaza2_full_order_log_deals.hpp"
#include "moex/plaza2/cgate/plaza2_replication_tables.hpp"
#include "plaza2_runtime_test_support.hpp"
#include "fake_cgate_control.hpp"
#include <array>
#include <algorithm>
#include <iostream>
using namespace moex::plaza2;
using namespace moex::plaza2_trade;
namespace f = test::fake;
static void anchored_deals(CgateSessionConfig config, f::Control& fake, f::Scenario scenario, bool explicit_lifenum) {
    using K = cgate::Plaza2ListenerEventKind;
    const std::array<std::int32_t, 1> ids{1001};
    cgate::Plaza2FullOrderLog book(ids, 1024, 256);
    cgate::FullOrderLogOpenInterest oi(ids);
    cgate::FullOrderLogDeals deals(ids);
    std::vector<K> events;
    std::vector<std::uint64_t> lives;
    unsigned derived{};
    oi.before_event = [&](const auto& event) {
        events.push_back(event.kind);
        if (event.kind == K::LifeNum)
            lives.push_back(event.unsigned_value);
        return deals.on_event(event);
    };
    oi.before_error = [&](const auto& error) { deals.on_error(error); };
    config.full_order_log_handler = &book;
    config.full_order_log_oi_handler = &oi;
    config.full_order_log_partitioned = true;
    config.trade_replay_from_pos_anchor = true;
    auto now = std::chrono::steady_clock::now();
    config.recovery_now = [&] { return now; };
    for (const auto code : {generated::StreamCode::kFortsTradeRepl, generated::StreamCode::kFortsPosRepl,
                            generated::StreamCode::kFortsUserorderbookRepl})
        config.private_streams.push_back(
            {code, cgate::required_replication_listener_settings(code), "mode=snapshot+online"});
    config.event_log = [&](std::string_view kind, std::string_view text) {
        if (kind == "full_order_log_deals_anchor") {
            ++derived;
            test::require(text.find("\"derived_initial_lifenum\":true") != std::string_view::npos,
                          "derived LifeNum provenance must be explicit");
        }
    };
    scenario.omit_anchored_deals_lifenum = true;
    scenario.deals_server_lifenum = explicit_lifenum ? 9 : 0;
    scenario.options[static_cast<std::size_t>(f::Option::DealsLifeNumBeforeOpen)] = explicit_lifenum ? "1" : "";
    fake.configure(scenario);
    CgateSession session(config);
    test::require(!session.start(), "anchored DEALS start");
    auto pump = [&] {
        for (unsigned i = 0; i < 50; ++i)
            test::require(!session.poll(false), "anchored DEALS poll");
    };
    pump();
    const auto expected = explicit_lifenum ? 9u : 7u;
    const auto snapshot = deals.snapshot(1001);
    test::require(snapshot.online && snapshot.valid && snapshot.lifenum == expected && oi.online(),
                  "anchored DEALS without native LifeNum must reach ONLINE with its confirmed epoch");
    test::require(!lives.empty() && lives.front() == expected &&
                      std::find(events.begin(), events.end(), K::LifeNum) <
                          std::find(events.begin(), events.end(), K::Open),
                  "confirmed LifeNum must reach the capture/trade chain before OPEN and rows");
    test::require(derived == (explicit_lifenum ? 0u : 1u),
                  "server LifeNum takes precedence; derive only the omitted notification");
    const auto settings = fake.listener_open_settings(generated::StreamCode::kFortsDealsRepl);
    test::require(settings.find(";lifenum=7;rev.deal=") != std::string::npos,
                  "public DEALS must retain the committed POS revision anchor");
    if (!explicit_lifenum) {
        fake.enqueue({.kind = f::EventKind::Close, .stream_code = generated::StreamCode::kFortsDealsRepl});
        pump();
        now += std::chrono::seconds(2);
        pump();
        test::require(deals.snapshot(1001).online && derived == 2,
                      "reopen must reestablish an omitted LifeNum instead of reusing stale state");
        fake.enqueue(
            {.kind = f::EventKind::LifeNum, .stream_code = generated::StreamCode::kFortsDealsRepl, .value = 0});
        pump();
        test::require(!deals.snapshot(1001).valid, "zero LifeNum remains a fatal DEALS validation error");
        now += std::chrono::seconds(2);
        pump();
        fake.enqueue(
            {.kind = f::EventKind::LifeNum, .stream_code = generated::StreamCode::kFortsDealsRepl, .value = 9});
        pump();
        test::require(!deals.snapshot(1001).valid, "a later different LifeNum still requires a fresh snapshot");
    }
    test::require(!session.stop(), "anchored DEALS stop");
}
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
        anchored_deals(c, fake, scenario, false);
        anchored_deals(c, fake, scenario, true);
        test::remove_tree(root);
        std::cout << "FOL-only OI routing and null-handler isolation passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
