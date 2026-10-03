#pragma once
#include "moex/connector_host/order_manager.hpp"
#include "fixtures/cgate99_messages.hpp"
#include <cstring>
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void reload_missing_order_regression() {
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    for (const bool present : {false, true}) {
        Fixture f;
        auto manager = f.manager();
        check(manager.place({.client_order_id = "reload", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "reload seed refused");
        f.poll(manager, 0);
        manager.on_reply(f.sent.back().id, {.msgid = 179, .order_id = 99001}, OrderManager::Clock::time_point{});
        plaza2::private_state::OwnOrderSnapshot snapshot;
        snapshot.public_order_id = snapshot.private_order_id = 99001;
        snapshot.sess_id = 100;
        snapshot.isin_id = 42;
        snapshot.dir = 1;
        snapshot.ext_id = manager.orders().at("reload").ext_id;
        snapshot.client_code = present ? "ABCD001" : "OTHER01";
        snapshot.from_user_book = true;
        snapshot.public_action = snapshot.private_action = 1;
        snapshot.price = "100";
        snapshot.public_amount = snapshot.public_amount_rest = snapshot.private_amount = snapshot.private_amount_rest =
            2;
        manager.invalidate_execution_baselines();
        manager.observe_orders(std::span(&snapshot, 1), true);
        manager.reconcile_snapshot(std::span(&snapshot, 1), 1700000100);
        if (present) {
            f.poll(manager, 1000);
            check(manager.orders().at("reload").state == OrderState::Working && f.sent.size() == 1,
                  "present current USERORDERBOOK order was subjected to absence recovery");
            continue;
        }
        check(manager.orders().at("reload").state == OrderState::Unknown && manager.orders().at("reload").order_id == 0,
              "missing owned order remained Working after complete reload");
        f.poll(manager, 1000);
        check(f.sent.size() == 2 && f.sent.back().kind == plaza2_trade::Plaza2TradeCommandKind::DelUserOrders,
              "missing reloaded order did not enter scoped ext_id absence recovery");
        official_cgate99::DelUserOrders wire{};
        std::memcpy(&wire, f.sent.back().payload.data(), sizeof(wire));
        check(wire.ext_id == manager.orders().at("reload").ext_id && wire.isin_id == 42 && wire.buy_sell == 3,
              "reload absence recovery was not scoped to exact owned ext_id/instrument");
        manager.on_reply(f.sent.back().id, {.msgid = 186, .num_orders = 0}, OrderManager::Clock::time_point{}, 5);
        manager.prove_absence(1700000161, false);
        check(manager.orders().at("reload").state == OrderState::Unknown,
              "offline TRADE incorrectly proved reload absence");
        manager.prove_absence(1700000159, true);
        check(manager.orders().at("reload").state == OrderState::Unknown,
              "old heartbeat incorrectly proved reload absence");
        manager.prove_absence(1700000161, true);
        check(manager.orders().at("reload").state == OrderState::Cancelled,
              "complete zero-cancel reply and fresh heartbeat did not resolve reload absence");
    }
    Fixture f;
    auto manager = f.manager();
    check(manager.place({.client_order_id = "reload-move", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
          "reload Move seed refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.back().id, {.msgid = 179, .order_id = 99002}, OrderManager::Clock::time_point{});
    check(manager.move("reload-move", "101", 2).empty(), "reload Move refused");
    f.poll(manager, 0);
    manager.invalidate_execution_baselines();
    manager.reconcile_snapshot({}, 1700000100);
    f.poll(manager, 1000);
    check(manager.orders().at("reload-move").state == OrderState::Unknown && manager.operator_action_required() &&
              f.sent.size() == 2,
          "missing uncertain Move was silently left Working or triggered an unrequested cancel");
}
} // namespace moex::connector_host
