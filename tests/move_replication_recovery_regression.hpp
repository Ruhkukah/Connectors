#pragma once
#include "moex/connector_host/order_manager.hpp"
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void move_replication_recovery_regression() {
    namespace ps = plaza2::private_state;
    using Kind = plaza2_trade::Plaza2TradeCommandKind;
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    for (const bool system100 : {false, true})
        for (const bool new_first : {false, true})
            for (const bool rows_before_timeout : {false, true}) {
                Fixture fixture;
                auto manager = fixture.manager();
                check(manager.place({.client_order_id = "lost-move", .isin_id = 42, .price = "100", .quantity = 3})
                          .empty(),
                      "lost Move seed refused");
                fixture.poll(manager, 0);
                manager.on_reply(fixture.sent.back().id, {.msgid = 179, .order_id = 97001},
                                 OrderManager::Clock::time_point{});
                ps::OwnOrderSnapshot old;
                old.public_order_id = old.private_order_id = 97001;
                old.sess_id = 100;
                old.isin_id = 42;
                old.client_code = "ABCD001";
                old.dir = 1;
                old.ext_id = manager.orders().at("lost-move").ext_id;
                old.price = "100";
                old.public_amount = old.public_amount_rest = 3;
                old.public_action = 1;
                old.from_trade_repl = true;
                manager.observe_orders(std::span(&old, 1));
                check(manager.move("lost-move", "101", 3).empty(), "lost Move admission refused");
                fixture.poll(manager, 1000);
                const auto move = fixture.sent.back().id;
                auto replacement = old;
                replacement.public_order_id = replacement.private_order_id = 97002;
                replacement.prevorder_id = 97001;
                replacement.price = "101";
                old.public_amount_rest = 0;
                old.public_action = 0;
                const auto rows = [&] {
                    if (new_first) {
                        manager.observe_orders(std::span(&replacement, 1));
                        manager.observe_orders(std::span(&old, 1));
                    } else {
                        manager.observe_orders(std::span(&old, 1));
                        manager.observe_orders(std::span(&replacement, 1));
                    }
                };
                if (rows_before_timeout)
                    rows();
                if (system100)
                    manager.on_reply(move, {.msgid = 100, .message = "reply missing"},
                                     OrderManager::Clock::time_point{});
                else
                    manager.on_timeout(move, OrderManager::Clock::time_point{});
                if (!rows_before_timeout)
                    rows();
                fixture.poll(manager, 180000);
                check(fixture.sent.size() == 2 && manager.orders().at("lost-move").order_id == 97002 &&
                          manager.orders().at("lost-move").state == OrderState::Working &&
                          manager.orders().at("lost-move").request.price == "101" &&
                          !manager.orders().at("lost-move").cancel_requested &&
                          !manager.orders().contains("recovered:100:97002"),
                      "lost Move was mass-cancelled or failed to adopt the exact replacement link");
                manager.on_reply(move, {.msgid = 176, .order_id1 = 97002}, OrderManager::Clock::time_point{});
                check(manager.move("lost-move", "102", 4).empty(), "resolved Move correlation blocked a later Move");
            }
    // An accepted recovery186 waits for evidence and never sends another186.
    Fixture fixture;
    auto manager = fixture.manager();
    check(manager.place({.client_order_id = "accepted186", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
          "accepted186 seed refused");
    fixture.poll(manager, 0);
    manager.on_timeout(fixture.sent.back().id, OrderManager::Clock::time_point{});
    fixture.poll(manager, 1000);
    check(fixture.sent.back().kind == Kind::DelUserOrders, "accepted186 recovery was not posted");
    manager.on_reply(fixture.sent.back().id, {.msgid = 186, .num_orders = 0}, OrderManager::Clock::time_point{});
    for (int i = 1; i <= 10; ++i)
        fixture.poll(manager, i * 120000);
    check(fixture.sent.size() == 2, "accepted per-order186 was resent while waiting for TRADE evidence");
    manager.prove_absence(1800000000, true);
    check(manager.orders().at("accepted186").state == OrderState::Cancelled,
          "accepted186 no longer permits a genuine committed absence proof");
}
} // namespace moex::connector_host
