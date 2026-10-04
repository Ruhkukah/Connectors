#pragma once

#include "moex/connector_host/order_manager.hpp"

#include <algorithm>
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void scoped_unresolved_admission_regression() {
    namespace ps = plaza2::private_state;
    namespace tr = plaza2_trade;
    const auto check = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    const auto time = [](std::int64_t ms) { return OrderManager::Clock::time_point{} + std::chrono::milliseconds(ms); };
    {
        Fixture fixture;
        fixture.config.reply_timeout = std::chrono::milliseconds(100);
        auto manager = fixture.manager();
        check(manager.place({.client_order_id = "unresolved", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "scoped uncertainty seed refused");
        check(manager.place({.client_order_id = "known-other", .isin_id = 43, .price = "100", .quantity = 2}).empty(),
              "other instrument seed refused");
        fixture.poll(manager, 0);
        const auto unresolved_add = fixture.sent[0].id;
        manager.on_reply(fixture.sent[1].id, {.msgid = 179, .order_id = 99243}, time(0));
        manager.on_timeout(unresolved_add, time(100));
        ps::OwnOrderSnapshot foreign;
        foreign.public_order_id = foreign.private_order_id = 99244;
        foreign.sess_id = 100;
        foreign.isin_id = 42;
        foreign.client_code = "ABCD001";
        foreign.login_from = "another-login";
        foreign.price = "100";
        foreign.ext_id = manager.orders().at("unresolved").ext_id;
        foreign.public_amount = foreign.public_amount_rest = 2;
        foreign.public_action = foreign.dir = 1;
        foreign.from_trade_repl = true;
        manager.observe_orders(std::span(&foreign, 1));
        check(manager.operator_action_required(), "identity mismatch did not raise an operator alert");
        check(!manager.place({.client_order_id = "blocked", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
              "unresolved instrument admitted new risk");
        check(manager.move("known-other", "101", 2).empty(), "one unresolved instrument blocked another's Move");
        check(manager.place({.client_order_id = "other-add", .isin_id = 43, .price = "100", .quantity = 1}).empty(),
              "one unresolved instrument blocked another's Add");
        fixture.poll(manager, 101);
        check(fixture.sent.size() == 4 && fixture.sent[2].kind == tr::Plaza2TradeCommandKind::MoveOrder &&
                  fixture.sent[3].kind == tr::Plaza2TradeCommandKind::AddOrder,
              "unaffected instrument commands did not reach the publisher");
        check(manager.orders().at("unresolved").state == OrderState::Unknown &&
                  manager.orders().at("unresolved").remaining == 2 && manager.operator_action_required(),
              "unaffected admission cleared unresolved exposure or its alert");
        manager.set_kill_switch(true);
        check(!manager.place({.client_order_id = "killed-other", .isin_id = 43, .price = "100", .quantity = 1}).empty(),
              "instrument-scoped alert bypassed the global kill switch");
        manager.set_kill_switch(false);
        manager.on_reply(unresolved_add, {.msgid = 179, .order_id = 99242}, time(101));
        auto terminal = foreign;
        terminal.public_order_id = terminal.private_order_id = 99242;
        terminal.login_from = "owner-login";
        terminal.public_amount_rest = terminal.public_action = 0;
        manager.observe_orders(std::span(&terminal, 1));
        check(!manager.operator_action_required() &&
                  manager.place({.client_order_id = "resolved", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
              "authoritative terminal proof did not restore the affected instrument");
    }
    {
        Fixture fixture;
        fixture.config.reply_timeout = std::chrono::milliseconds(100);
        fixture.config.sole_instance = true;
        auto manager = fixture.manager();
        check(manager.cancel_all(42).empty(), "empty scoped bulk seed refused");
        fixture.poll(manager, 0);
        for (int attempt = 0; attempt < 3; ++attempt) {
            manager.on_timeout(fixture.sent.back().id, time(attempt * 1100 + 100));
            fixture.poll(manager, attempt * 1100 + 1100);
        }
        const auto exhausted_id = fixture.sent.back().id;
        check(manager.operator_action_required() && manager.cancellations_pending(),
              "empty bulk lost unresolved intent");
        check(!manager.place({.client_order_id = "bulk-blocked", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
              "exhausted bulk instrument admitted new risk");
        check(manager.place({.client_order_id = "bulk-other", .isin_id = 43, .price = "100", .quantity = 1}).empty(),
              "empty unresolved bulk blocked another instrument");
        fixture.poll(manager, 3301);
        check(fixture.sent.back().kind == tr::Plaza2TradeCommandKind::AddOrder && manager.operator_action_required(),
              "unaffected bulk admission cleared its unresolved warning");
        manager.on_reply(exhausted_id, {.msgid = 186, .num_orders = 0}, time(3301), 1);
        manager.observe_trade_commit(2);
        check(!manager.operator_action_required() &&
                  manager.place({.client_order_id = "bulk-resolved", .isin_id = 42, .price = "100", .quantity = 1})
                      .empty(),
              "definitive bulk reply and later committed view did not restore its instrument");
    }
}
} // namespace moex::connector_host
