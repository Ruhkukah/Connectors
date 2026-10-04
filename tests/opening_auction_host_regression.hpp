#pragma once
#include "late_move_host_regression.hpp"
namespace moex::connector_host {
inline void opening_auction_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                            const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    using enum gen::FieldCode;
    const auto isin = config.isin_ids.front();
    auto now = OrderManager::Clock::time_point{};
    config.session.recovery_now = [&] { return now; };
    config.orders.sole_instance = true;
    config.journal_path = root / "opening-auction.ndjson";
    config.identity_state_path = root / "opening-auction.state";
    control.configure({.suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = config.orders.broker_code + config.orders.client_code,
                       .session_id = 321});
    CgateTradingHost host(config);
    bootstrap(host);
    const auto state = [&](int phase) {
        now += std::chrono::seconds(1);
        control.enqueue({.kind = fake::EventKind::Begin, .stream_code = gen::StreamCode::kFortsInstrumentstateRepl});
        control.enqueue({.stream_code = gen::StreamCode::kFortsInstrumentstateRepl,
                         .table_code = gen::TableCode::kFortsInstrumentstateReplInstrumentState,
                         .revision = 80000 + phase,
                         .fields = {integer(kFortsInstrumentstateReplInstrumentStateIsinId, isin),
                                    integer(kFortsInstrumentstateReplInstrumentStatePublicState, phase)}});
        control.enqueue({.kind = fake::EventKind::Commit, .stream_code = gen::StreamCode::kFortsInstrumentstateRepl});
        require(!host.poll(), "opening auction state poll");
    };
    state(6);
    const auto before = control.commands().size();
    require(host.place({.client_order_id = "auction-day", .isin_id = isin, .price = "103000", .quantity = 2}).empty(),
            "opening auction refused permitted Day Add");
    require(control.commands().size() == before + 1 && control.commands().back().name == "AddOrder",
            "opening auction Day Add failed native command admission");
    require(!host.poll(), "opening auction accepted Add poll");
    require(!host.place({.client_order_id = "auction-ioc",
                         .isin_id = isin,
                         .type = plaza2_trade::Plaza2TradeOrderType::Ioc,
                         .price = "103000",
                         .quantity = 1})
                 .empty(),
            "opening auction admitted prohibited IOC");
    require(!host.move("auction-day", "103000", 2).empty(), "opening auction admitted prohibited Move");
    require(control.commands().size() == before + 1, "refused auction IOC/Move reached native publisher");
    for (const int phase : {0, 2, 5, 6, 8, 9}) {
        state(phase);
        const auto count = control.commands().size();
        require(host.cancel_all(isin).empty(), "risk-reducing cancel refused in non-entry phase");
        require(control.commands().size() == count + 1 && control.commands().back().name == "DelUserOrders",
                "non-entry phase blocked native risk-reducing cancellation");
    }
    require(!host.stop(), "opening auction host stop");
}
} // namespace moex::connector_host
