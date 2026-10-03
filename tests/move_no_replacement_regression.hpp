#pragma once
#include "moex/connector_host/order_manager.hpp"
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void move_no_replacement_regression() {
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    Fixture fixture;
    auto manager = fixture.manager();
    check(manager.place({.client_order_id = "move-filled", .isin_id = 42, .price = "100", .quantity = 3}).empty(),
          "Move fill fixture Add refused");
    fixture.poll(manager, 0);
    manager.on_reply(fixture.sent.back().id, {.msgid = 179, .order_id = 96001}, OrderManager::Clock::time_point{});
    plaza2::private_state::OwnTradeSnapshot fill;
    fill.id_deal = 9601;
    fill.sess_id = 100;
    fill.isin_id = 42;
    fill.code_buy = "ABCD001";
    fill.public_order_id_buy = fill.private_order_id_buy = 96001;
    fill.amount = 1;
    manager.observe_trades(std::span(&fill, 1));
    check(!manager.move("move-filled", "101", 1).empty(),
          "regime3 Move with quantity equal to all executions reached the queue");
    check(manager.move("move-filled", "101", 2).empty(), "valid regime3 replacement refused");
    fixture.poll(manager, 1000);
    manager.on_reply(fixture.sent.back().id, {.msgid = 176, .order_id1 = 0}, OrderManager::Clock::time_point{});
    check(manager.orders().at("move-filled").state == OrderState::PendingCancel,
          "definite no-replacement176 was treated as an uncertain Move");
    fixture.poll(manager, 180000);
    check(fixture.sent.size() == 2 && !manager.operator_action_required(),
          "definite no-replacement176 started unrequested cancel recovery");
    plaza2::private_state::OwnOrderSnapshot row;
    row.public_order_id = row.private_order_id = 96001;
    row.sess_id = 100;
    row.isin_id = 42;
    row.client_code = "ABCD001";
    row.dir = 1;
    row.public_amount = 3;
    row.public_action = 0;
    row.from_trade_repl = true;
    manager.observe_orders(std::span(&row, 1));
    check(manager.orders().at("move-filled").state == OrderState::Cancelled &&
              manager.orders().at("move-filled").executed == 1,
          "TRADE did not settle the no-replacement Move while retaining actual fills");
}
} // namespace moex::connector_host
