#pragma once

#include "moex/connector_host/order_manager.hpp"
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void strict_lost_add_regression() {
    namespace ps = plaza2::private_state;
    namespace tr = plaza2_trade;
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    // A colliding ext_id is not ownership proof, before or after the timeout.
    for (const bool early : {false, true})
        for (int mismatch = 0; mismatch < 6; ++mismatch) {
            Fixture fixture;
            fixture.config.login_from = mismatch == 5 ? "" : "owner-login";
            fixture.config.reply_timeout = std::chrono::milliseconds(100);
            auto manager = fixture.manager();
            check(
                manager.place({.client_order_id = "strict-add", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
                "strict Add admission failed");
            fixture.poll(manager, 0);
            ps::OwnOrderSnapshot row;
            row.public_order_id = row.private_order_id = 94001;
            row.sess_id = 100;
            row.isin_id = 42;
            row.client_code = mismatch == 3 ? "OTHER01" : "ABCD001";
            row.login_from = mismatch == 0 ? "other-login" : mismatch == 4 ? "" : "owner-login";
            row.price = mismatch == 1 ? "101" : "100";
            row.public_amount = row.public_amount_rest = mismatch == 2 ? 3 : 2;
            row.public_action = 1;
            row.ext_id = manager.orders().at("strict-add").ext_id;
            row.dir = 1;
            row.from_trade_repl = true;
            if (early)
                manager.observe_orders(std::span(&row, 1));
            manager.on_timeout(fixture.sent.front().id,
                               OrderManager::Clock::time_point{} + std::chrono::milliseconds(100));
            if (!early)
                manager.observe_orders(std::span(&row, 1));
            fixture.poll(manager, 1000);
            const auto& original = manager.orders().at("strict-add");
            check(original.order_id == 0 && original.state == OrderState::Unknown && original.operator_action_required,
                  "lost Add adopted a row without exact login/client/price/quantity ownership");
            check(fixture.sent.size() == 1 && fixture.sent.front().kind == tr::Plaza2TradeCommandKind::AddOrder,
                  "mismatched lost Add dispatched an automatic foreign-ID or ext-ID cancellation");
        }
    // Wire price formatting differs from the submitted text. A reduced amount
    // remains valid when the separately committed own fill completes its total.
    {
        Fixture positive;
        positive.config.login_from = "owner-login";
        auto adopted = positive.manager();
        check(adopted.place({.client_order_id = "exact-partial", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "exact partial Add refused");
        positive.poll(adopted, 0);
        ps::OwnOrderSnapshot row;
        row.public_order_id = row.private_order_id = 94003;
        row.sess_id = 100;
        row.isin_id = 42;
        row.client_code = "ABCD001";
        row.login_from = "owner-login";
        row.price = "100.00000";
        row.public_amount = 2;
        row.public_amount_rest = row.public_action = row.dir = 1;
        row.ext_id = adopted.orders().at("exact-partial").ext_id;
        row.from_trade_repl = true;
        adopted.observe_orders(std::span(&row, 1));
        ps::OwnTradeSnapshot trade;
        trade.id_deal = 84003;
        trade.sess_id = 100;
        trade.isin_id = 42;
        trade.code_buy = "ABCD001";
        trade.public_order_id_buy = trade.private_order_id_buy = 94003;
        trade.amount = 1;
        trade.price = "100";
        adopted.observe_trades(std::span(&trade, 1));
        row.public_amount = 1;
        adopted.observe_orders(std::span(&row, 1));
        adopted.on_timeout(positive.sent.front().id, OrderManager::Clock::time_point{});
        const auto& original = adopted.orders().at("exact-partial");
        check(original.order_id == 94003 && original.executed == 1 && original.remaining == 1 &&
                  !original.operator_action_required && !adopted.orders().contains("recovered:100:94003"),
              "strict matching rejected numeric price equality or a quantity completed by its own actual fill");
    }
    // Deployment-assigned ranges do not move in response to another login's
    // cursor, and exhausted entry capacity still permits risk reduction.
    Fixture fixture;
    fixture.config.login_from = "owner-login";
    fixture.config.ext_id_begin = 100;
    fixture.config.ext_id_end = 102;
    auto manager = fixture.manager();
    ps::OwnOrderSnapshot foreign;
    foreign.public_order_id = foreign.private_order_id = 94002;
    foreign.sess_id = 100;
    foreign.isin_id = 42;
    foreign.client_code = "ABCD001";
    foreign.login_from = "other-login";
    foreign.price = "100";
    foreign.public_amount = foreign.public_amount_rest = 1;
    foreign.public_action = foreign.dir = 1;
    foreign.ext_id = INT32_MAX;
    foreign.from_trade_repl = true;
    manager.observe_orders(std::span(&foreign, 1));
    for (int index = 0; index < 3; ++index) {
        const auto key = "range-" + std::to_string(index);
        check(manager.place({.client_order_id = key, .isin_id = 42, .price = "100", .quantity = 1}).empty(),
              "foreign row exhausted an instance's ext_id range");
        check(manager.orders().at(key).ext_id == 100 + index,
              "instance allocated an ext_id outside its declared range");
    }
    check(!manager.place({.client_order_id = "range-exhausted", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
          "instance allocated beyond its ext_id range");
    fixture.config.next_ext_id = 1000; // Crash reservation may skip the end.
    auto exhausted = fixture.manager();
    exhausted.observe_orders(std::span(&foreign, 1), true);
    check(!exhausted.place({.client_order_id = "exhausted", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
          "crash-reserved cursor escaped the instance range");
    check(exhausted.cancel("recovered:100:94002").empty(), "ext_id range exhaustion disabled known-ID cancellation");
    fixture.poll(exhausted, 2000);
    check(fixture.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder,
          "ext_id range exhaustion prevented risk reduction");
}
} // namespace moex::connector_host
