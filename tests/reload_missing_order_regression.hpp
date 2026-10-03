#pragma once
#include "moex/connector_host/order_manager.hpp"
#include "fixtures/cgate99_messages.hpp"
#include <cstring>
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void reload_missing_order_regression() {
    namespace ps = plaza2::private_state;
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    const auto snapshot = [](const ManagedOrder& order, std::int64_t id) {
        ps::OwnOrderSnapshot row;
        row.public_order_id = row.private_order_id = id;
        row.sess_id = 100;
        row.isin_id = 42;
        row.dir = 1;
        row.ext_id = order.ext_id;
        row.client_code = "ABCD001";
        row.login_from = "owner-login";
        row.from_trade_repl = true;
        row.public_action = row.private_action = 1;
        row.price = order.request.price;
        row.public_amount = row.public_amount_rest = row.private_amount = row.private_amount_rest = 2;
        return row;
    };
    for (const bool present : {false, true}) {
        Fixture f;
        auto manager = f.manager();
        check(manager.place({.client_order_id = "reload", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "reload seed refused");
        f.poll(manager, 120000);
        manager.on_reply(f.sent.back().id, {.msgid = 179, .order_id = 99001}, OrderManager::Clock::time_point{});
        auto row = snapshot(manager.orders().at("reload"), 99001);
        row.from_trade_repl = false;
        row.from_user_book = true;
        row.client_code = present ? "ABCD001" : "OTHER01";
        manager.invalidate_execution_baselines();
        manager.observe_orders(std::span(&row, 1), true);
        manager.reconcile_snapshot(std::span(&row, 1), 1700000000);
        manager.prove_absence(1700000061, true); // Past Add's old startup clock, before the actual send.
        manager.prove_absence(1700000180, true); // The exact safety margin is not proof.
        manager.prove_absence(1700001000, false);
        f.poll(manager, 120001);
        check(manager.orders().at("reload").state == OrderState::Working &&
                  manager.orders().at("reload").order_id == 99001 && f.sent.size() == 1 && manager.queued() == 0,
              "complete reload cancelled an acknowledged Add before its TRADE watermark matured");
        if (present) {
            manager.prove_absence(1700001000, true);
            f.poll(manager, 121000);
            check(manager.orders().at("reload").state == OrderState::Working && f.sent.size() == 1,
                  "present current USERORDERBOOK order was subjected to absence recovery");
            continue;
        }
        manager.prove_absence(1700000181, true);
        check(manager.orders().at("reload").state == OrderState::Unknown &&
                  manager.orders().at("reload").order_id == 99001,
              "mature missing-order proof erased its known exchange identity");
        f.poll(manager, 181000);
        check(f.sent.size() == 2 && f.sent.back().kind == plaza2_trade::Plaza2TradeCommandKind::DelUserOrders,
              "mature missing-order proof did not enter scoped ext_id absence recovery");
        official_cgate99::DelUserOrders wire{};
        std::memcpy(&wire, f.sent.back().payload.data(), sizeof(wire));
        check(wire.ext_id == manager.orders().at("reload").ext_id && wire.isin_id == 42 && wire.buy_sell == 3,
              "reload absence recovery was not scoped to exact owned ext_id/instrument");
        manager.on_reply(f.sent.back().id, {.msgid = 186, .num_orders = 0}, OrderManager::Clock::time_point{}, 5);
        manager.prove_absence(1700000181, false);
        check(manager.orders().at("reload").state == OrderState::Unknown,
              "offline TRADE incorrectly proved reload absence");
        manager.prove_absence(1700000180, true);
        check(manager.orders().at("reload").state == OrderState::Unknown,
              "old heartbeat incorrectly proved reload absence");
        manager.prove_absence(1700000181, true);
        check(manager.orders().at("reload").state == OrderState::Cancelled &&
                  manager.orders().at("reload").order_id == 99001,
              "zero-cancel reply and mature heartbeat did not resolve known-ID reload absence");
    }
    {
        Fixture f;
        auto manager = f.manager();
        check(manager.place({.client_order_id = "reappears", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "reappearing Add seed refused");
        f.poll(manager, 60000);
        manager.on_reply(f.sent.back().id, {.msgid = 179, .order_id = 99002}, OrderManager::Clock::time_point{});
        manager.reconcile_snapshot({}, 0); // LifeNum clears the heartbeat until a fresh commit.
        const auto row = snapshot(manager.orders().at("reappears"), 99002);
        manager.observe_orders(std::span(&row, 1));
        manager.prove_absence(1700001000, true);
        f.poll(manager, 61000);
        check(manager.orders().at("reappears").state == OrderState::Working && f.sent.size() == 1,
              "replicated order remained staged as missing after it reappeared");
    }
    {
        Fixture f;
        auto manager = f.manager();
        ManagedOrder recovered;
        recovered.request = {.isin_id = 42, .price = "100", .quantity = 2};
        recovered.ext_id = 77;
        const auto row = snapshot(recovered, 99003);
        manager.observe_orders(std::span(&row, 1), true);
        const auto key = std::string("recovered:100:99003");
        manager.reconcile_snapshot({}, 0);
        manager.prove_absence(0, true);
        manager.prove_absence(1700000100, true); // First positive watermark anchors a row with no local send time.
        manager.prove_absence(1700000160, true);
        check(manager.orders().at(key).state == OrderState::Working && manager.queued() == 0,
              "recovered missing order treated its first committed watermark as immediate absence proof");
        manager.prove_absence(1700000161, true);
        f.poll(manager, 161000);
        check(manager.orders().at(key).state == OrderState::Unknown && manager.orders().at(key).order_id == 99003 &&
                  f.sent.size() == 1 && f.sent.back().kind == plaza2_trade::Plaza2TradeCommandKind::DelUserOrders,
              "zero-watermark reload never resumed a conservative recovered-order absence proof");
    }
    {
        Fixture f;
        auto manager = f.manager();
        ManagedOrder recovered;
        recovered.request = {.isin_id = 42, .price = "100", .quantity = 2};
        const auto row = snapshot(recovered, 99006);
        manager.observe_orders(std::span(&row, 1), true);
        manager.reconcile_snapshot({}, 1700000100);
        manager.prove_absence(1700000161, true);
        f.poll(manager, 161000);
        const auto& order = manager.orders().at("recovered:100:99006");
        check(order.state == OrderState::Unknown && order.order_id == 99006 && order.operator_action_required &&
                  f.sent.empty() && manager.queued() == 0,
              "missing recovered ext_id=0 order triggered a wildcard recovery cancellation");
    }
    for (const bool replied : {false, true}) {
        Fixture f;
        auto manager = f.manager();
        check(manager.place({.client_order_id = "reload-move", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "reload Move seed refused");
        f.poll(manager, 0);
        manager.on_reply(f.sent.back().id, {.msgid = 179, .order_id = 99004}, OrderManager::Clock::time_point{});
        const auto old = snapshot(manager.orders().at("reload-move"), 99004);
        manager.observe_orders(std::span(&old, 1));
        check(manager.move("reload-move", "101", 2).empty(), "reload Move refused");
        f.poll(manager, 120000);
        if (replied)
            manager.on_reply(f.sent.back().id, {.msgid = 176, .order_id1 = 99005}, OrderManager::Clock::time_point{});
        else
            manager.on_timeout(f.sent.back().id, OrderManager::Clock::time_point{});
        manager.invalidate_execution_baselines();
        manager.reconcile_snapshot({}, 0);
        manager.prove_absence(1700000061, true);
        manager.prove_absence(1700000180, true);
        f.poll(manager, 120001);
        check(manager.orders().at("reload-move").sent_utc_seconds == 1700000120 && f.sent.size() == 2 &&
                  !manager.operator_action_required() &&
                  manager.orders().at("reload-move").state == (replied ? OrderState::Working : OrderState::Unknown),
              "reload absence used Add time or cancelled an in-flight Move before its margin");
        if (replied) {
            const auto replacement = snapshot(manager.orders().at("reload-move"), 99005);
            manager.observe_orders(std::span(&replacement, 1));
            manager.prove_absence(1700001000, true);
            f.poll(manager, 121000);
            check(manager.orders().at("reload-move").state == OrderState::Working && f.sent.size() == 2,
                  "late replacement replication was subjected to an unrequested reload cancellation");
        } else {
            manager.prove_absence(1700000181, true);
            f.poll(manager, 181000);
            check(manager.orders().at("reload-move").state == OrderState::Unknown &&
                      manager.orders().at("reload-move").order_id == 99004 && manager.operator_action_required() &&
                      f.sent.size() == 2,
                  "missing uncertain Move was treated as unapplied or triggered an unrequested cancel");
        }
    }
}
} // namespace moex::connector_host
