#pragma once
#include "late_move_host_regression.hpp"
namespace moex::connector_host {
inline void reload_missing_host_regression(TradingHostConfig config, const plaza2::test::fake::Control& control,
                                           const std::filesystem::path& root) {
    using namespace private_delta_host_detail;
    using enum gen::FieldCode;
    const auto isin = config.isin_ids.front();
    auto now = OrderManager::Clock::time_point{};
    config.session.recovery_now = [&] { return now; };
    config.utc_now = [] { return std::int64_t{1700000100}; };
    config.journal_path = root / "reload-missing.ndjson";
    config.identity_state_path = root / "reload-missing.state";
    control.configure({.suppress_auto_replies = true,
                       .suppress_initial_orders = true,
                       .zero_position = true,
                       .client_code = config.orders.broker_code + config.orders.client_code,
                       .session_id = 321});
    CgateTradingHost host(config);
    bootstrap(host);
    require(host.place({.client_order_id = "missing-after-reload", .isin_id = isin, .price = "103000", .quantity = 2})
                .empty(),
            "native missing reload Add seed refused");
    official_cgate99::FORTS_MSG179 accepted{};
    accepted.order_id = 99100;
    late_move_host_detail::reply(control, control.commands().back().user_id, 179, accepted);
    require(!host.poll(), "native missing reload Add reply poll");
    require(late_move_host_detail::logical_order(host, "missing-after-reload").find("\"state\":\"Working\"") !=
                std::string::npos,
            "native missing reload seed not Working");
    const auto before = control.commands().size();
    control.enqueue({.kind = fake::EventKind::LifeNum, .stream_code = gen::StreamCode::kFortsTradeRepl, .value = 2});
    control.enqueue({.kind = fake::EventKind::Online, .stream_code = gen::StreamCode::kFortsTradeRepl});
    for (int i = 0; i < 5; ++i)
        require(!host.poll(), "native missing reload rebuild poll");
    require(host.status().find("\"reconstructing\":false") != std::string::npos &&
                late_move_host_detail::logical_order(host, "missing-after-reload").find("\"state\":\"Unknown\"") !=
                    std::string::npos,
            "native complete LifeNum reload left missing tracked order Working");
    require(control.commands().size() == before + 1 && control.commands().back().name == "DelUserOrders",
            "native reload did not issue exact ext_id absence recovery");
    require(!host.stop(), "native missing reload stop");
}
} // namespace moex::connector_host
