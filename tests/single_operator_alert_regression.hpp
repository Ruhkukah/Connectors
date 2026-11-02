#pragma once

#include "moex/connector_host/order_manager.hpp"

#include <stdexcept>

namespace moex::connector_host {
template <class Order> constexpr bool has_separate_duo_alert = requires(Order order) { order.duo_exhaustion_alert; };

template <class Fixture> void single_operator_alert_regression() {
    namespace ps = plaza2::private_state;
    const auto check = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    check(!has_separate_duo_alert<ManagedOrder>, "DUO exhaustion still has a separate per-order operator-alert flag");
    const auto time = [](std::int64_t ms) { return OrderManager::Clock::time_point{} + std::chrono::milliseconds(ms); };
    for (const bool fill_conflict : {false, true})
        for (const bool late_reply : {false, true}) {
            Fixture fixture;
            fixture.config.sole_instance = true;
            auto manager = fixture.manager();
            check(manager.place({.client_order_id = "stale-absence", .isin_id = 42, .price = "100", .quantity = 2})
                      .empty(),
                  "stale absence seed refused");
            fixture.poll(manager, 0);
            const auto add = fixture.sent.front().id;
            manager.on_timeout(add, time(100));
            fixture.poll(manager, 100);
            const auto recovery = fixture.sent.back().id;
            if (!late_reply)
                manager.on_reply(recovery, {.msgid = 186, .num_orders = 0}, time(100));
            check(manager.orders().at("stale-absence").absence_reply == !late_reply,
                  "absence fixture was not established in the requested reply order");
            ps::OwnOrderSnapshot row;
            row.public_order_id = row.private_order_id = 99442;
            row.sess_id = 100;
            row.isin_id = 42;
            row.client_code = "ABCD001";
            row.login_from = fill_conflict ? "owner-login" : "another-login";
            row.ext_id = manager.orders().at("stale-absence").ext_id;
            row.price = "100";
            row.public_amount = row.public_amount_rest = 2;
            row.public_action = row.dir = 1;
            row.from_trade_repl = true;
            if (fill_conflict)
                manager.on_reply(add, {.msgid = 179, .order_id = 99442}, time(101));
            manager.observe_orders(std::span(&row, 1));
            if (fill_conflict) {
                ps::OwnTradeSnapshot trade;
                trade.id_deal = 99443;
                trade.sess_id = 100;
                trade.isin_id = 42;
                trade.public_order_id_buy = trade.private_order_id_buy = 99442;
                trade.code_buy = "ABCD001";
                trade.price = "100";
                trade.amount = 1;
                manager.observe_trades(std::span(&trade, 1));
                trade.amount = 2;
                manager.observe_trades(std::span(&trade, 1));
                manager.reconcile_snapshot({}, 1700000100);
            }
            manager.on_reply(recovery, {.msgid = 186, .num_orders = 0}, time(102));
            manager.prove_absence(1700000200, true);
            const auto& unresolved = manager.orders().at("stale-absence");
            check(unresolved.state == OrderState::Unknown && unresolved.operator_action_required &&
                      !unresolved.absence_reply && unresolved.remaining > 0,
                  "stale absence reply overrode a later identity or own-fill conflict");
            if (!fill_conflict) {
                check(manager.cancel_all(42).empty(), "broad post-conflict cancellation refused");
                const auto before_bulk = fixture.sent.size();
                fixture.poll(manager, 103);
                check(fixture.sent.size() > before_bulk &&
                          fixture.sent[before_bulk].kind == plaza2_trade::Plaza2TradeCommandKind::DelUserOrders,
                      "post-conflict broad cancellation did not actually post");
                manager.on_reply(fixture.sent[before_bulk].id, {.msgid = 186, .num_orders = 0}, time(103), 1);
                manager.observe_trade_commit(2);
                manager.prove_absence(1700000300, true);
                check(manager.orders().at("stale-absence").state == OrderState::Unknown &&
                          manager.orders().at("stale-absence").operator_action_required &&
                          !manager.orders().at("stale-absence").absence_reply,
                      "broad186 zero overrode an unresolved identity conflict");
            }
        }
}
} // namespace moex::connector_host
