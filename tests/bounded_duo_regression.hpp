#pragma once
#include "moex/connector_host/order_manager.hpp"
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void bounded_duo_regression() {
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    const auto time = [](std::int64_t ms) { return OrderManager::Clock::time_point{} + std::chrono::milliseconds(ms); };
    {
        namespace tr = plaza2_trade;
        Fixture f;
        f.config.reply_timeout = std::chrono::milliseconds(100);
        f.config.sole_instance = true;
        auto manager = f.manager();
        check(manager.place({.client_order_id = "mixed-known", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "mixed fallback seed Add refused");
        f.poll(manager, 0);
        manager.on_reply(f.sent.back().id, {.msgid = 179, .order_id = 78004}, time(0));
        check(manager.cancel_all(42).empty(), "mixed fallback bulk refused");
        f.poll(manager, 0);
        manager.on_timeout(f.sent.back().id, time(100));
        f.poll(manager, 1100);
        for (int rejection = 0; rejection < 3; ++rejection) {
            const auto now = f.ms;
            manager.on_reply(f.sent.back().id, {.msgid = 186, .code = 17}, time(now));
            f.poll(manager, now + (rejection == 0 ? 1000 : 2000));
        }
        check(f.sent.size() == 6 && f.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder &&
                  manager.operator_action_required(),
              "retained uncertain bulk gate prevented known-ID individual fallback");
        auto now = f.ms;
        manager.on_reply(f.sent.back().id, {.msgid = 99, .penalty_remain = 1000}, time(now));
        f.poll(manager, now + 999);
        check(f.sent.size() == 6, "known-ID mixed fallback ignored flood pacing");
        f.poll(manager, now + 1000);
        check(f.sent.size() == 7 && f.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder,
              "retained bulk gate dropped known-ID flood retry");
        now = f.ms;
        manager.on_timeout(f.sent.back().id, time(now + 100));
        f.poll(manager, now + 1100);
        check(f.sent.size() == 8 && f.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder,
              "retained bulk gate dropped known-ID timeout retry");
        now = f.ms;
        manager.on_reply(f.sent.back().id, {.msgid = 177, .code = 17}, time(now));
        f.poll(manager, now + 1000);
        check(f.sent.size() == 9 && f.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder,
              "bulk failures consumed the individual known-ID business budget");
        plaza2::private_state::OwnOrderSnapshot terminal;
        terminal.public_order_id = terminal.private_order_id = 78004;
        terminal.sess_id = 100;
        terminal.isin_id = 42;
        terminal.client_code = "ABCD001";
        terminal.dir = 1;
        terminal.public_amount = 2;
        terminal.price = "100";
        terminal.from_trade_repl = true;
        terminal.trade_repl_commit_sequence = 1;
        manager.observe_orders(std::span(&terminal, 1));
        check(!manager.orders().at("mixed-known").operator_action_required && manager.operator_action_required() &&
                  !manager
                       .place({.client_order_id = "mixed-after-terminal", .isin_id = 42, .price = "100", .quantity = 1})
                       .empty(),
              "individual terminal proof cleared earlier uncertain broad intent");
        check(manager.cancel_all(42).empty(), "mixed known fallback renewal refused");
        f.poll(manager, f.ms + 1000);
        check(f.sent.back().kind == tr::Plaza2TradeCommandKind::DelUserOrders,
              "explicit renewed bulk request lost its command family");
        manager.on_reply(f.sent.back().id, {.msgid = 186, .num_orders = 0}, time(f.ms), 1);
        manager.observe_trade_commit(2);
        check(!manager.operator_action_required() && !manager.cancellations_pending(),
              "renewed bulk proof did not settle mixed known fallback intent");
    }
    for (const bool system_result : {false, true}) {
        Fixture f;
        f.config.reply_timeout = std::chrono::milliseconds(100);
        f.config.sole_instance = true;
        auto manager = f.manager();
        check(manager.cancel_all(42).empty(), "mixed bulk request refused");
        f.poll(manager, 0);
        const auto first_uid = f.sent.back().id;
        if (system_result)
            manager.on_reply(first_uid, {.msgid = 100, .code = 1}, time(100));
        else
            manager.on_timeout(first_uid, time(100));
        f.poll(manager, 1100);
        for (int rejection = 0; rejection < 3; ++rejection) {
            const auto now = f.ms;
            manager.on_reply(f.sent.back().id, {.msgid = 186, .code = 17}, time(now));
            f.poll(manager, now + (rejection == 0 ? 1000 : 2000));
        }
        check(f.sent.size() == 4 && manager.queued() == 0 && manager.operator_action_required() &&
                  manager.cancellations_pending(),
              "business rejection exhaustion discarded an earlier uncertain broad DUO");
        manager.observe_trade_commit(10);
        check(
            !manager.place({.client_order_id = "mixed-new-risk", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
            "empty account view exposed a later Add to a previously uncertain broad DUO");
        manager.on_reply(f.sent.back().id, {.msgid = 186, .num_orders = 0}, time(f.ms), 10);
        manager.on_reply(first_uid, {.msgid = 186, .num_orders = 0}, time(f.ms), 10);
        manager.observe_trade_commit(11);
        check(manager.operator_action_required() && manager.cancellations_pending(),
              "late retired or definitively rejected DUO UID cleared unresolved earlier broad intent");
        check(std::any_of(f.log.begin(), f.log.end(),
                          [](const auto& line) {
                              return line.starts_with("unresolved{") &&
                                     line.find("explicit renewal required") != std::string::npos;
                          }),
              "mixed broad uncertainty did not report the explicit-renewal limitation");
        check(manager.cancel_all(42).empty(), "mixed bulk explicit renewal refused");
        f.poll(manager, f.ms + 1000);
        check(f.sent.size() == 5 && manager.operator_action_required(), "mixed renewal did not preserve its alert");
        manager.on_reply(f.sent.back().id, {.msgid = 186, .num_orders = 0}, time(f.ms), 11);
        manager.observe_trade_commit(12);
        check(!manager.operator_action_required() && !manager.cancellations_pending(),
              "fresh explicit bulk renewal and committed empty view did not resolve mixed intent");
    }
    {
        Fixture f;
        f.config.sole_instance = true;
        auto manager = f.manager();
        check(manager.cancel_all(42).empty(), "bulk intent readiness seed refused");
        f.poll(manager, 0);
        check(!manager.place({.client_order_id = "same-instrument", .isin_id = 42, .price = "100", .quantity = 1})
                   .empty(),
              "unacknowledged broad DUO admitted a later same-instrument order");
        check(manager.place({.client_order_id = "other-instrument", .isin_id = 43, .price = "100", .quantity = 1})
                  .empty(),
              "unexhausted broad DUO unnecessarily blocked another instrument");
        f.poll(manager, 1);
        check(f.sent.size() == 2 && manager.orders().size() == 1 && manager.orders().contains("other-instrument"),
              "broad cancellation readiness submitted refused same-instrument risk");
        manager.on_reply(f.sent.front().id, {.msgid = 186, .num_orders = 0}, time(1));
        manager.observe_trade_commit(1);
        check(manager.place({.client_order_id = "after-reconciliation", .isin_id = 42, .price = "100", .quantity = 1})
                  .empty(),
              "acknowledged and reconciled broad cancellation kept its instrument gate");
    }
    for (const bool system_result : {false, true}) {
        Fixture f;
        f.config.reply_timeout = std::chrono::milliseconds(100);
        f.config.sole_instance = true;
        auto manager = f.manager();
        check(manager.cancel_all(42).empty(), "bounded empty bulk cancel refused");
        f.poll(manager, 0);
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto id = f.sent.back().id;
            const auto at = attempt * 1100 + 100;
            if (system_result)
                manager.on_reply(id, {.msgid = 100, .code = 1}, time(at));
            else
                manager.on_timeout(id, time(at));
            f.poll(manager, at + 1000);
        }
        check(f.sent.size() == 3 && manager.queued() == 0 && manager.operator_action_required() &&
                  manager.cancellations_pending(),
              "unacknowledged bulk DUO exceeded three uncertain outcomes or lost its alert/correlation");
        for (int wait = 1; wait <= 5; ++wait)
            f.poll(manager, 10000 * wait);
        check(f.sent.size() == 3 &&
                  !manager.place({.client_order_id = "after-bulk-limit", .isin_id = 42, .price = "100", .quantity = 1})
                       .empty(),
              "exhausted bulk DUO resumed sends or admitted new risk");
        manager.on_reply(f.sent.back().id, {.msgid = 186, .num_orders = 0}, time(f.ms), 1);
        manager.observe_trade_commit(2);
        check(manager.operator_action_required() && manager.cancellations_pending(),
              "retired exhausted DUO reply cleared unresolved bulk intent");
        check(manager.cancel_all(42).empty(), "retired exhausted bulk renewal refused");
        f.poll(manager, f.ms + 1000);
        manager.on_reply(f.sent.back().id, {.msgid = 186, .num_orders = 0}, time(f.ms), 2);
        manager.observe_trade_commit(3);
        check(!manager.operator_action_required() && !manager.cancellations_pending(),
              "fresh accepted186 and complete empty TRADE view did not settle stopped bulk scope");
        check(std::count_if(f.log.begin(), f.log.end(),
                            [](const auto& value) { return value.starts_with("timeout{"); }) == 3,
              "DUO uncertain outcomes lost correlated timeout audit records");
    }
    for (const int resolution : {0, 1, 2, 3}) {
        Fixture f;
        f.config.reply_timeout = std::chrono::milliseconds(100);
        auto manager = f.manager();
        check(manager.place({.client_order_id = "bounded-recovery", .isin_id = 42, .price = "100", .quantity = 2})
                  .empty(),
              "bounded recovery Add refused");
        f.poll(manager, 0);
        manager.on_timeout(f.sent.front().id, time(100));
        f.poll(manager, 100);
        for (int attempt = 0; attempt < 3; ++attempt) {
            manager.on_timeout(f.sent.back().id, time(200 + attempt * 1100));
            f.poll(manager, 1200 + attempt * 1100);
        }
        const auto& order = manager.orders().at("bounded-recovery");
        check(f.sent.size() == 4 && manager.queued() == 0 && order.state == OrderState::Unknown &&
                  order.operator_action_required && order.remaining == 2,
              "ext-scoped recovery DUO was unlimited or discarded uncertain Add exposure");
        const auto old_id = f.sent.back().id;
        if (resolution == 3) {
            plaza2::private_state::OwnOrderSnapshot terminal;
            terminal.public_order_id = terminal.private_order_id = 78001;
            terminal.sess_id = 100;
            terminal.isin_id = 42;
            terminal.client_code = "ABCD001";
            terminal.login_from = "owner-login";
            terminal.dir = 1;
            terminal.ext_id = order.ext_id;
            terminal.price = "100";
            terminal.public_amount = 2;
            terminal.from_trade_repl = true;
            manager.observe_orders(std::span(&terminal, 1));
            check(manager.orders().at("bounded-recovery").order_id == 78001 &&
                      manager.orders().at("bounded-recovery").state == OrderState::Cancelled &&
                      !manager.operator_action_required() && !manager.cancellations_pending(),
                  "exact owned native terminal identity did not settle exhaustion-created scoped uncertainty");
            continue;
        }
        if (resolution != 1) {
            if (resolution == 2) {
                plaza2::private_state::OwnOrderSnapshot foreign;
                foreign.public_order_id = foreign.private_order_id = 78001;
                foreign.sess_id = 100;
                foreign.isin_id = 42;
                foreign.client_code = "ABCD001";
                foreign.login_from = "foreign-login";
                foreign.dir = 1;
                foreign.ext_id = order.ext_id;
                foreign.price = "100";
                foreign.public_amount = foreign.public_amount_rest = 2;
                foreign.public_action = 1;
                foreign.from_trade_repl = true;
                manager.observe_orders(std::span(&foreign, 1));
            }
            manager.on_reply(old_id, {.msgid = 186, .num_orders = 0}, time(f.ms));
            manager.prove_absence(1700000060, true);
            check(manager.orders().at("bounded-recovery").state == OrderState::Unknown &&
                      manager.operator_action_required(),
                  "late scoped186 bypassed the TRADE absence safety margin");
            manager.prove_absence(1700000061, true);
            if (resolution == 2) {
                check(manager.orders().at("bounded-recovery").state == OrderState::Unknown &&
                          manager.operator_action_required() &&
                          !manager.orders().at("bounded-recovery").duo_exhaustion_alert,
                      "late scoped186 cleared an unrelated lost-Add identity warning");
                f.poll(manager, 10000);
                check(f.sent.size() == 4, "foreign matching ext_id evidence introduced an automatic cancellation");
                continue;
            }
            check(manager.orders().at("bounded-recovery").state == OrderState::Unknown &&
                      manager.operator_action_required(),
                  "retired exhausted scoped186 supplied false absence proof");
            check(manager.cancel("bounded-recovery").empty(), "retired scoped recovery renewal refused");
            f.poll(manager, 10000);
            manager.on_reply(f.sent.back().id, {.msgid = 186, .num_orders = 0}, time(f.ms));
            manager.prove_absence(1700000061, true);
            check(manager.orders().at("bounded-recovery").state == OrderState::Cancelled &&
                      !manager.operator_action_required() && !manager.cancellations_pending(),
                  "renewed scoped186zero and mature TRADE proof did not settle exhausted recovery");
            continue;
        }
        check(manager.cancel("bounded-recovery").empty(), "explicit scoped DUO renewal refused");
        f.poll(manager, 10000);
        check(f.sent.size() == 5 && f.sent.back().id != old_id && manager.operator_action_required(),
              "explicit scoped renewal did not obtain a fresh bounded attempt or cleared its alert");
    }
    {
        Fixture f;
        f.config.reply_timeout = std::chrono::milliseconds(100);
        f.config.sole_instance = true;
        auto manager = f.manager();
        check(manager.cancel_all(42).empty(), "bulk renewal seed refused");
        f.poll(manager, 0);
        for (int attempt = 0; attempt < 3; ++attempt) {
            manager.on_timeout(f.sent.back().id, time(attempt * 1100 + 100));
            f.poll(manager, attempt * 1100 + 1100);
        }
        check(manager.cancel_all(42).empty() && manager.operator_action_required(),
              "explicit bulk renewal refused or cleared the prior unresolved warning");
        manager.observe_trade_commit(1);
        f.poll(manager, 10000);
        check(f.sent.size() == 4 && manager.cancellations_pending() && manager.operator_action_required(),
              "empty committed view retired a fresh bulk intent before it was posted");
        manager.on_timeout(f.sent.back().id, time(10100));
        f.poll(manager, 11100);
        check(f.sent.size() == 5, "explicit bulk renewal reused the exhausted uncertain-outcome budget");
        manager.on_reply(f.sent.back().id, {.msgid = 186, .num_orders = 0}, time(11100), 1);
        manager.observe_trade_commit(2);
        check(!manager.cancellations_pending() && !manager.operator_action_required(),
              "renewed bulk intent did not settle after its own reply and committed empty view");
    }
    {
        Fixture f;
        f.config.reply_timeout = std::chrono::milliseconds(100);
        f.config.sole_instance = true;
        auto manager = f.manager();
        check(manager.place({.client_order_id = "bulk-terminal", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "bulk terminal proof Add refused");
        f.poll(manager, 0);
        manager.on_reply(f.sent.back().id, {.msgid = 179, .order_id = 78002}, time(0));
        check(manager.cancel_all(42).empty(), "bulk terminal cancellation refused");
        f.poll(manager, 0);
        for (int attempt = 0; attempt < 3; ++attempt) {
            manager.on_timeout(f.sent.back().id, time(attempt * 1100 + 100));
            f.poll(manager, attempt * 1100 + 1100);
        }
        const auto last_uid = f.sent.back().id;
        plaza2::private_state::OwnOrderSnapshot terminal;
        terminal.public_order_id = terminal.private_order_id = 78002;
        terminal.sess_id = 100;
        terminal.isin_id = 42;
        terminal.client_code = "ABCD001";
        terminal.dir = 1;
        terminal.public_amount = 2;
        terminal.price = "100";
        terminal.from_trade_repl = true;
        terminal.trade_repl_commit_sequence = 1;
        manager.observe_orders(std::span(&terminal, 1));
        check(manager.orders().at("bulk-terminal").state == OrderState::Cancelled &&
                  !manager.orders().at("bulk-terminal").operator_action_required &&
                  manager.operator_action_required() &&
                  !manager.place({.client_order_id = "late-broad-risk", .isin_id = 42, .price = "100", .quantity = 1})
                       .empty(),
              "current native terminal orders cleared an unacknowledged broad DUO and allowed new risk");
        manager.on_reply(last_uid, {.msgid = 186, .num_orders = 1}, time(f.ms), 1);
        check(manager.operator_action_required(), "late broad186 bypassed post-reply TRADE reconciliation");
        manager.observe_trade_commit(2);
        check(manager.operator_action_required() && manager.cancellations_pending(),
              "retired broad186 settled earlier unconfirmed cancellation");
        check(manager.cancel_all(42).empty(), "terminal exhausted bulk renewal refused");
        f.poll(manager, f.ms + 1000);
        manager.on_reply(f.sent.back().id, {.msgid = 186, .num_orders = 0}, time(f.ms), 2);
        manager.observe_trade_commit(3);
        check(!manager.operator_action_required() && !manager.cancellations_pending(),
              "renewed broad186 with committed terminal view did not settle retained bulk uncertainty");
    }
}
} // namespace moex::connector_host
