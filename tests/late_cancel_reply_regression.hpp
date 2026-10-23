#pragma once

#include "moex/connector_host/order_manager.hpp"

#include <algorithm>
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void late_cancel_reply_regression() {
    namespace ps = plaza2::private_state;
    namespace tr = plaza2_trade;
    const auto check = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    const auto time = [](std::int64_t ms) { return OrderManager::Clock::time_point{} + std::chrono::milliseconds(ms); };
    const auto logged = [](const Fixture& fixture, std::string_view event, std::uint32_t id,
                           std::string_view field = {}) {
        const auto prefix = std::string(event) + "{\"user_id\":" + std::to_string(id);
        return std::any_of(fixture.log.begin(), fixture.log.end(), [&](const auto& line) {
            return line.starts_with(prefix) &&
                   (line.size() == prefix.size() || line[prefix.size()] == ',' || line[prefix.size()] == '}') &&
                   (field.empty() || line.find(field) != std::string::npos);
        });
    };
    const auto native = [](const ManagedOrder& order, std::int64_t id, bool cancelled) {
        ps::OwnOrderSnapshot row;
        row.public_order_id = row.private_order_id = id;
        row.sess_id = order.sess_id;
        row.isin_id = order.request.isin_id;
        row.client_code = "ABCD001";
        row.login_from = "owner-login";
        row.price = order.request.price;
        row.public_amount = order.request.quantity;
        row.public_amount_rest = cancelled ? 0 : order.request.quantity;
        row.ext_id = order.ext_id;
        row.public_action = cancelled ? 0 : 1;
        row.dir = 1;
        row.from_trade_repl = true;
        row.trade_repl_commit_sequence = 1;
        return row;
    };
    const auto seed_cancel = [&](Fixture& fixture, OrderManager& manager, const std::string& key, std::int64_t order_id,
                                 std::int64_t ms = 0) {
        check(manager.place({.client_order_id = key, .isin_id = 42, .price = "100", .quantity = 1}).empty(),
              "late cancel Add fixture refused");
        fixture.poll(manager, ms);
        manager.on_reply(fixture.sent.back().id, {.msgid = 179, .order_id = order_id}, time(ms));
        check(manager.cancel(key).empty(), "late cancel fixture refused");
        fixture.poll(manager, ms);
        check(fixture.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder, "late cancel was not posted");
        const auto uid = fixture.sent.back().id;
        const auto terminal = native(manager.orders().at(key), order_id, true);
        manager.observe_orders(std::span(&terminal, 1));
        check(manager.orders().at(key).state == OrderState::Cancelled, "committed cancel not applied");
        return uid;
    };
    {
        // Native F8 ordering: committed cancellation is about 0.8ms before 177.
        Fixture fixture;
        auto manager = fixture.manager();
        const auto uid = seed_cancel(fixture, manager, "late-cancel", 98301);
        manager.on_reply(uid, {.msgid = 177, .code = 0, .amount = 1}, time(1), 1);
        check(logged(fixture, "late_reply", uid, "\"expected_msgid\":177") &&
                  logged(fixture, "late_reply", uid, "\"acknowledged\":true") && !logged(fixture, "unknown_reply", uid),
              "committed cancellation before reply was still logged unknown");
        manager.on_reply(uid, {.msgid = 177, .code = 0, .amount = 1}, time(2), 2);
        check(logged(fixture, "late_reply", uid, "\"duplicate\":true"), "duplicate late cancel lost its identity");
        fixture.poll(manager, 3);
        check(fixture.sent.size() == 2 && manager.queued() == 0 &&
                  manager.orders().at("late-cancel").state == OrderState::Cancelled &&
                  manager.orders().at("late-cancel").remaining == 0 &&
                  manager.orders().at("late-cancel").executed == 0 && !manager.operator_action_required(),
              "late cancel reply changed terminal accounting or resent intent");
    }
    {
        // A duplicate official reply after the ordinary reply-then-commit
        // ordering is still a duplicate, not the first late acknowledgment.
        Fixture fixture;
        auto manager = fixture.manager();
        check(manager.place({.client_order_id = "reply-before-terminal", .isin_id = 42, .price = "100", .quantity = 1})
                  .empty(),
              "reply-first seed refused");
        fixture.poll(manager, 0);
        manager.on_reply(fixture.sent.back().id, {.msgid = 179, .order_id = 98305}, time(0));
        check(manager.cancel("reply-before-terminal").empty(), "reply-first cancel refused");
        fixture.poll(manager, 1);
        const auto uid = fixture.sent.back().id;
        manager.on_reply(uid, {.msgid = 177, .code = 0}, time(2));
        const auto terminal = native(manager.orders().at("reply-before-terminal"), 98305, true);
        manager.observe_orders(std::span(&terminal, 1));
        manager.on_reply(uid, {.msgid = 177, .code = 0}, time(3));
        check(logged(fixture, "late_reply", uid, "\"duplicate\":true"),
              "already acknowledged cancel was logged as a first late reply");
        check(fixture.sent.size() == 2 && !manager.cancellations_pending(),
              "duplicate acknowledged reply restored cancellation intent");
    }
    {
        Fixture fixture;
        auto manager = fixture.manager();
        const auto uid = seed_cancel(fixture, manager, "mismatch-cancel", 98302);
        manager.on_reply(uid, {.msgid = 186, .code = 0}, time(1));
        manager.on_reply(uid, {.msgid = 177, .code = 17}, time(2));
        check(logged(fixture, "late_reply_mismatch", uid, "\"acknowledged\":false") &&
                  !logged(fixture, "late_reply", uid),
              "wrong-kind or rejected late cancel was attributed as successful");
        manager.on_reply(uid, {.msgid = 177, .code = 0}, time(3));
        check(logged(fixture, "late_reply", uid, "\"duplicate\":false"),
              "mismatched late reply consumed the exact completed command identity");
        manager.on_reply(uid + 10000, {.msgid = 177, .code = 0}, time(4));
        check(logged(fixture, "unknown_reply", uid + 10000), "unmatched reply was falsely attributed");
        fixture.poll(manager, 5);
        check(fixture.sent.size() == 2 && manager.orders().at("mismatch-cancel").state == OrderState::Cancelled &&
                  !manager.operator_action_required(),
              "mismatched late reply changed risk or retried cancellation");
    }
    {
        // Scoped recovery DUO has a logical order key; instrument-wide DUO is
        // deliberately still gated on its own 186 and subsequent TRADE commit.
        Fixture fixture;
        fixture.config.reply_timeout = std::chrono::milliseconds(100);
        auto manager = fixture.manager();
        check(
            manager.place({.client_order_id = "late-scoped-duo", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
            "late DUO Add fixture refused");
        fixture.poll(manager, 0);
        manager.on_timeout(fixture.sent.back().id, time(100));
        fixture.poll(manager, 101);
        check(fixture.sent.back().kind == tr::Plaza2TradeCommandKind::DelUserOrders,
              "late DUO fixture did not post scoped recovery");
        const auto uid = fixture.sent.back().id;
        const auto terminal = native(manager.orders().at("late-scoped-duo"), 98303, true);
        manager.observe_orders(std::span(&terminal, 1));
        manager.on_reply(uid, {.msgid = 186, .code = 0, .num_orders = 1}, time(102), 1);
        check(logged(fixture, "late_reply", uid, "\"expected_msgid\":186") && !logged(fixture, "unknown_reply", uid),
              "scoped completed DUO lost late reply correlation");
        fixture.poll(manager, 103);
        check(fixture.sent.size() == 2 && manager.orders().at("late-scoped-duo").state == OrderState::Cancelled &&
                  !manager.operator_action_required(),
              "late scoped DUO reply changed accounting or resent intent");
    }
    {
        Fixture fixture;
        fixture.config.sole_instance = true;
        auto manager = fixture.manager();
        check(manager.place({.client_order_id = "global-duo", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
              "global DUO Add fixture refused");
        fixture.poll(manager, 0);
        manager.on_reply(fixture.sent.back().id, {.msgid = 179, .order_id = 98305}, time(0));
        check(manager.cancel_all(42).empty(), "global DUO fixture refused");
        fixture.poll(manager, 1);
        const auto uid = fixture.sent.back().id;
        check(fixture.sent.back().kind == tr::Plaza2TradeCommandKind::DelUserOrders, "global DUO was not posted");
        const auto terminal = native(manager.orders().at("global-duo"), 98305, true);
        manager.observe_orders(std::span(&terminal, 1));
        check(manager.cancellations_pending(), "terminal order prematurely cleared global DUO reply gate");
        manager.on_reply(uid, {.msgid = 186, .code = 0, .num_orders = 1}, time(2), 1);
        check(manager.cancellations_pending() && !logged(fixture, "late_reply", uid) &&
                  !logged(fixture, "unknown_reply", uid),
              "global DUO lost normal reply correlation or later-commit gate");
        manager.observe_trade_commit(2);
        check(!manager.cancellations_pending() && fixture.sent.size() == 2,
              "global DUO reply plus later TRADE commit did not complete normally");
    }
    {
        Fixture fixture;
        fixture.config.reply_timeout = std::chrono::milliseconds(100);
        auto manager = fixture.manager();
        const auto uid = seed_cancel(fixture, manager, "expired-cancel", 98304);
        manager.on_reply(uid, {.msgid = 177, .code = 0}, time(100));
        check(logged(fixture, "unknown_reply", uid) && !logged(fixture, "late_reply", uid),
              "expired completed-cancel correlation remained live");
    }
    {
        Fixture fixture;
        fixture.config.max_commands_per_second = 3000;
        auto manager = fixture.manager();
        std::uint32_t first{}, last{};
        for (int i = 0; i < 1025; ++i) {
            last = seed_cancel(fixture, manager, "bounded-late-cancel-" + std::to_string(i), 100000 + i);
            if (i == 0)
                first = last;
        }
        manager.on_reply(first, {.msgid = 177, .code = 0}, time(1));
        manager.on_reply(last, {.msgid = 177, .code = 0}, time(1));
        check(logged(fixture, "unknown_reply", first) && logged(fixture, "late_reply", last),
              "completed cancel history is unbounded or evicts the newest identity");
    }
}
} // namespace moex::connector_host
