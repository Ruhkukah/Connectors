#pragma once

#include "moex/connector_host/order_manager.hpp"
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void lost_add_recovery_regression() {
    namespace ps = plaza2::private_state;
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    // Replication can precede the timeout or arrive during recovery.
    for (const bool before_timeout : {false, true}) {
        Fixture fixture;
        fixture.config.reply_timeout = std::chrono::milliseconds(100);
        auto manager = fixture.manager();
        check(manager.place({.client_order_id = "lost-add", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "lost Add admission failed");
        fixture.poll(manager, 0);
        const auto add = fixture.sent.front().id;
        const auto ext = manager.orders().at("lost-add").ext_id;
        ps::OwnOrderSnapshot row;
        row.public_order_id = row.private_order_id = 91001;
        row.sess_id = 100;
        row.isin_id = 42;
        row.client_code = "ABCD001";
        row.login_from = "owner-login";
        row.price = "100";
        row.ext_id = ext;
        row.dir = 1;
        row.public_amount = 2;
        row.public_amount_rest = 0;
        row.public_action = 2;
        row.from_trade_repl = true;
        const auto replication = [&] {
            manager.observe_orders(std::span(&row, 1));
            ps::OwnTradeSnapshot trade;
            trade.id_deal = 81001;
            trade.sess_id = 100;
            trade.isin_id = 42;
            trade.code_buy = "ABCD001";
            trade.public_order_id_buy = trade.private_order_id_buy = 91001;
            trade.amount = 2;
            trade.price = "100";
            manager.observe_trades(std::span(&trade, 1));
        };
        if (before_timeout)
            replication();
        manager.on_timeout(add, OrderManager::Clock::time_point{} + std::chrono::milliseconds(100));
        if (!before_timeout)
            replication();
        fixture.poll(manager, 101);
        for (const auto& sent : fixture.sent)
            if (sent.kind == plaza2_trade::Plaza2TradeCommandKind::DelUserOrders)
                manager.on_reply(sent.id, {.msgid = 186, .num_orders = 0},
                                 OrderManager::Clock::time_point{} + std::chrono::milliseconds(101));
        manager.prove_absence(1800000000, true);
        const auto& original = manager.orders().at("lost-add");
        check(original.order_id == 91001 && original.state == OrderState::Filled && original.executed == 2 &&
                  original.remaining == 0 && original.last_error.find("NotFound") == std::string::npos &&
                  !manager.orders().contains("recovered:100:91001"),
              "lost Add was called NotFound instead of retaining its exact owned replicated fill");
        manager.on_reply(add, {.msgid = 179, .order_id = 91001},
                         OrderManager::Clock::time_point{} + std::chrono::milliseconds(102));
        replication();
        check(manager.orders().at("lost-add").state == OrderState::Filled &&
                  manager.orders().at("lost-add").executed == 2,
              "late Add confirmation resurrected a filled order or counted its fill twice");
    }
    // Every component of recovery identity is mandatory, even with equal ext_id.
    for (int mismatch = 0; mismatch < 4; ++mismatch) {
        Fixture fixture;
        auto manager = fixture.manager();
        check(manager.place({.client_order_id = "unmatched", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "unmatched Add admission failed");
        fixture.poll(manager, 0);
        ps::OwnOrderSnapshot row;
        row.public_order_id = row.private_order_id = 92001;
        row.sess_id = mismatch == 1 ? 101 : 100;
        row.isin_id = mismatch == 2 ? 43 : 42;
        row.client_code = mismatch == 0 ? "OTHER01" : "ABCD001";
        row.login_from = "owner-login";
        row.dir = mismatch == 3 ? 2 : 1;
        row.price = "100";
        row.ext_id = manager.orders().at("unmatched").ext_id;
        row.public_amount = row.public_amount_rest = 2;
        row.public_action = 1;
        row.from_trade_repl = true;
        manager.on_timeout(fixture.sent.front().id, OrderManager::Clock::time_point{});
        manager.observe_orders(std::span(&row, 1));
        check(manager.orders().at("unmatched").order_id == 0,
              "lost Add adopted an ext_id with mismatched ownership/session/contract/side");
    }
    Fixture fixture;
    auto manager = fixture.manager();
    check(manager.place({.client_order_id = "resting", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
          "resting Add admission failed");
    fixture.poll(manager, 0);
    const auto add = fixture.sent.front().id;
    manager.on_timeout(add, OrderManager::Clock::time_point{});
    ps::OwnOrderSnapshot row;
    row.public_order_id = row.private_order_id = 93001;
    row.sess_id = 100;
    row.isin_id = 42;
    row.client_code = "ABCD001";
    row.login_from = "owner-login";
    row.price = "100";
    row.ext_id = manager.orders().at("resting").ext_id;
    row.dir = 1;
    row.public_amount = row.public_amount_rest = 2;
    row.public_action = 1;
    row.from_trade_repl = true;
    manager.observe_orders(std::span(&row, 1));
    fixture.poll(manager, 1000);
    for (const auto& sent : fixture.sent)
        if (sent.kind == plaza2_trade::Plaza2TradeCommandKind::DelUserOrders)
            manager.on_reply(sent.id, {.msgid = 186, .num_orders = 0}, OrderManager::Clock::time_point{});
    manager.prove_absence(1800000000, true);
    check(manager.orders().at("resting").order_id == 93001 &&
              manager.orders().at("resting").state == OrderState::PendingCancel &&
              manager.orders().at("resting").remaining == 2,
          "lost Add called a replicated resting order absent");
    manager.on_reply(add, {.msgid = 179, .order_id = 93002}, OrderManager::Clock::time_point{});
    check(manager.orders().at("resting").order_id == 93001 &&
              manager.orders().at("resting").state == OrderState::Unknown && manager.operator_action_required() &&
              !manager.place({.client_order_id = "blocked", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
          "contradictory late Add confirmation rebound recovery instead of requiring the operator");
}
} // namespace moex::connector_host
