#pragma once

#include "moex/connector_host/order_manager.hpp"
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void move_not_applied_regression() {
    namespace ps = plaza2::private_state;
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    Fixture fixture;
    fixture.config.reply_timeout = std::chrono::milliseconds(100);
    fixture.config.risk.max_notional_scaled = 100000000;
    auto manager = fixture.manager();
    check(manager.place({.client_order_id = "move-unapplied", .isin_id = 42, .price = "100", .quantity = 3}).empty(),
          "unapplied Move seed refused");
    fixture.poll(manager, 0);
    manager.on_reply(fixture.sent.back().id, {.msgid = 179, .order_id = 95001}, OrderManager::Clock::time_point{});
    ps::OwnOrderSnapshot old;
    old.public_order_id = old.private_order_id = 95001;
    old.sess_id = 100;
    old.isin_id = 42;
    old.client_code = "ABCD001";
    old.login_from = "owner-login";
    old.ext_id = manager.orders().at("move-unapplied").ext_id;
    old.price = "100";
    old.public_amount = old.public_amount_rest = 3;
    old.public_action = old.dir = 1;
    old.from_trade_repl = true;
    manager.observe_orders(std::span(&old, 1));
    check(manager.move("move-unapplied", "200", 5).empty(), "unapplied Move admission refused");
    fixture.poll(manager, 120000);
    const auto move = fixture.sent.back().id;
    manager.on_timeout(move, OrderManager::Clock::time_point{} + std::chrono::milliseconds(120100));
    manager.observe_orders(std::span(&old, 1));
    check(manager.orders().at("move-unapplied").state == OrderState::Unknown,
          "unapplied Move discarded uncertainty before the committed watermark");
    manager.prove_absence(1700000061, true); // Beyond Add's margin, before Move.
    manager.prove_absence(1700000180, true); // Exact Move margin is insufficient.
    manager.prove_absence(1700001000, false);
    check(manager.orders().at("move-unapplied").state == OrderState::Unknown,
          "unapplied Move used Add time, an immature watermark, or offline data as its proof");
    check(!manager.place({.client_order_id = "reserved-risk", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
          "unresolved Move released its replacement exposure before proof");
    manager.prove_absence(1700000181, true);
    const auto& order = manager.orders().at("move-unapplied");
    check(order.state == OrderState::Working && order.order_id == 95001 && order.remaining == 3 &&
              order.request.price == "100" && order.request.quantity == 3 && !order.operator_action_required &&
              !order.cancel_requested,
          "committed watermark left a provably unapplied Move Unknown forever");
    check(manager.place({.client_order_id = "released-risk", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
          "unapplied Move retained its replacement exposure after proof");
    check(manager.move("move-unapplied", "101", 3).empty(),
          "proved-unapplied Move retained a blocking command correlation");
    std::size_t events{};
    for (const auto& line : fixture.log)
        events += line.starts_with("move_not_applied");
    check(events == 1 && fixture.sent.size() == 2, "unapplied Move was retried/cancelled or lacked one audit event");
    manager.on_reply(move, {.msgid = 176, .order_id1 = 95009}, OrderManager::Clock::time_point{});
    check(manager.orders().at("move-unapplied").order_id == 95001 && !manager.operator_action_required(),
          "retired delayed176 altered a proved-unapplied Move");

    for (const bool replacement_seen : {false, true}) {
        Fixture pending;
        auto unresolved = pending.manager();
        check(
            unresolved.place({.client_order_id = "linked-move", .isin_id = 42, .price = "100", .quantity = 3}).empty(),
            "linked Move seed refused");
        pending.poll(unresolved, 0);
        unresolved.on_reply(pending.sent.back().id, {.msgid = 179, .order_id = 95001},
                            OrderManager::Clock::time_point{});
        unresolved.observe_orders(std::span(&old, 1));
        check(unresolved.move("linked-move", "200", 5).empty(), "linked Move admission refused");
        pending.poll(unresolved, 120000);
        unresolved.on_timeout(pending.sent.back().id, OrderManager::Clock::time_point{});
        auto replacement = old;
        replacement.public_order_id = replacement.private_order_id = 95002;
        replacement.prevorder_id = 95001;
        replacement.price = "200";
        replacement.public_amount = replacement.public_amount_rest = 5;
        if (replacement_seen)
            unresolved.observe_orders(std::span(&replacement, 1));
        else {
            auto deleted = old;
            deleted.public_amount_rest = 0;
            deleted.public_action = 0;
            unresolved.observe_orders(std::span(&deleted, 1));
        }
        unresolved.prove_absence(1700001000, true);
        check(unresolved.orders().at("linked-move").state == OrderState::Unknown && pending.sent.size() == 2,
              "watermark resolved a Move with an absent old ID or a linked replacement already seen");
        if (replacement_seen) {
            auto deleted = old;
            deleted.public_amount_rest = 0;
            deleted.public_action = 0;
            unresolved.observe_orders(std::span(&deleted, 1));
            check(unresolved.orders().at("linked-move").order_id == 95002 &&
                      unresolved.orders().at("linked-move").state == OrderState::Working,
                  "linked replacement no longer resolves after its old-ID deletion");
        }
    }
}
} // namespace moex::connector_host
