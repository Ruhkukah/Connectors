#pragma once

#include "private_delta_host_regression.hpp"

namespace moex::connector_host {
inline void userbook_barrier_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                             const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    using enum gen::StreamCode;
    using enum gen::TableCode;
    using enum gen::FieldCode;
    fake::Scenario scenario{.suppress_initial_orders = true,
                            .zero_position = true,
                            .client_code = config.orders.broker_code + config.orders.client_code,
                            .session_id = 321};
    scenario.options[static_cast<std::size_t>(fake::Option::DelayUserorderbook)] = "1";
    control.configure(scenario);
    config.session.mode = plaza2_trade::CgateSessionMode::Live;
    config.journal_path = root / "userbook-barrier.ndjson";
    CgateTradingHost host(config);
    require(!host.start(), "USERORDERBOOK publication barrier startup");
    const auto poll = [&] {
        for (int i = 0; i < 20; ++i)
            require(!host.poll(), "USERORDERBOOK publication barrier poll");
    };
    const auto publication = [&](std::int64_t state, std::int64_t revision, std::int64_t life = 7) {
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = kFortsUserorderbookRepl});
        control.enqueue({.stream_code = kFortsUserorderbookRepl,
                         .table_code = kFortsUserorderbookReplInfo,
                         .revision = revision,
                         .fields = {integer(kFortsUserorderbookReplInfoPublicationState, state),
                                    integer(kFortsUserorderbookReplInfoTradesRev, revision),
                                    integer(kFortsUserorderbookReplInfoTradesLifenum, life)}});
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = kFortsUserorderbookRepl});
        control.enqueue({.kind = fake::EventKind::Online, .stream_code = kFortsUserorderbookRepl});
    };
    poll();
    publication(0, 2000);
    poll();
    require(host.status().find("\"reconstructing\":true") != std::string::npos &&
                !host.place(add("incomplete-book", config.isin_ids.front())).empty(),
            "incomplete USERORDERBOOK publication released reconstruction or admitted Add");
    const auto before = control.opens(kFortsTradeRepl);
    publication(1, 2000, 8);
    poll();
    require(host.status().find("\"reconstructing\":true") != std::string::npos &&
                control.opens(kFortsTradeRepl) == before,
            "different USERORDERBOOK/POS epochs opened TRADE or released reconstruction");
    publication(1, 2000);
    poll();
    require(host.status().find("\"reconstructing\":false") != std::string::npos &&
                host.status().find("\"order_entry_ready\":true") != std::string::npos &&
                control.opens(kFortsTradeRepl) == before + 1 &&
                control.trade_open_settings().find(";rev.orders_log=2000;") != std::string::npos &&
                control.trade_open_settings().find(";rev.deal=44;") != std::string::npos,
            "empty account failed catch-up from the completed order-book revision and independent POS anchor");
    publication(1, 4000);
    poll();
    require(control.opens(kFortsTradeRepl) == before + 1 &&
                host.status().find("\"reconstructing\":false") != std::string::npos,
            "live periodic USERORDERBOOK publication unnecessarily reopened TRADE");
    control.enqueue({.kind = fake::EventKind::Close, .stream_code = kFortsUserorderbookRepl});
    poll();
    require(host.status().find("\"reconstructing\":true") != std::string::npos,
            "USERORDERBOOK loss did not restore the reconstruction barrier");
    publication(0, 6000);
    poll();
    require(host.status().find("\"reconstructing\":true") != std::string::npos &&
                control.opens(kFortsTradeRepl) == before + 1,
            "incomplete recovered book used an older ONLINE proof");
    publication(1, 6000);
    poll();
    require(host.status().find("\"reconstructing\":false") != std::string::npos &&
                control.opens(kFortsTradeRepl) == before + 2 &&
                control.trade_open_settings().find(";rev.orders_log=6000;") != std::string::npos,
            "reconstruction failed to catch up exactly once from the newer completed order-book revision");
    require(!host.stop(), "USERORDERBOOK publication barrier stop");
}
} // namespace moex::connector_host
