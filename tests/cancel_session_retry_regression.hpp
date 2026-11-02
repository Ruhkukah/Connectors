#pragma once
#include "moex/connector_host/order_manager.hpp"
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void closed_session_refusal_retirement_regression() {
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    using Clock = OrderManager::Clock;
    for (const auto code : {3, 4, 66}) {
        Fixture fixture;
        fixture.config.sole_instance = true;
        auto manager = fixture.manager();
        check(manager.cancel_all(42).empty(), "refusal retirement seed refused");
        fixture.poll(manager, 0);
        manager.on_reply(fixture.sent.back().id, {.msgid = 186, .code = code}, Clock::time_point{});
        check(manager.cancellations_pending(), "session refusal was treated as an acknowledgement");
        check(manager.retire_closed_session_refusal(42) == (code == 3),
              "snapshot proof retired a non-code3 wait or failed a definite closed-session refusal");
        if (code == 3) {
            check(!manager.cancellations_pending() && fixture.sent.size() == 1,
                  "closed-session refusal completion posted a retry or retained its drained gate");
            check(std::any_of(fixture.log.begin(), fixture.log.end(),
                              [](const auto& line) {
                                  return line.find("refused_session_closed") != std::string::npos &&
                                         line.find("\"acknowledged\":false") != std::string::npos;
                              }),
                  "actual code3 refusal lacked its explicit non-acknowledgement record");
        }
    }
    {
        Fixture fixture;
        fixture.config.sole_instance = true;
        auto manager = fixture.manager();
        check(manager.cancel_all(42).empty(), "uncertain refusal seed refused");
        fixture.poll(manager, 0);
        manager.on_timeout(fixture.sent.back().id, Clock::time_point{});
        fixture.poll(manager, 1000);
        manager.on_reply(fixture.sent.back().id, {.msgid = 186, .code = 3},
                         Clock::time_point{} + std::chrono::seconds(1));
        check(!manager.retire_closed_session_refusal(42) && manager.cancellations_pending(),
              "later code3 refusal erased an earlier uncertain mass-cancel outcome");
    }
    {
        Fixture fixture;
        fixture.config.sole_instance = true;
        auto manager = fixture.manager();
        check(
            manager.place({.client_order_id = "unresolved-add", .isin_id = 43, .price = "100", .quantity = 1}).empty(),
            "pending Add retirement fixture refused");
        fixture.poll(manager, 0); // Deliberately leave this native Add without a reply.
        check(manager.cancel_all(42).empty(), "pending Add refusal seed refused");
        fixture.poll(manager, 0);
        manager.on_reply(fixture.sent.back().id, {.msgid = 186, .code = 3}, Clock::time_point{});
        check(!manager.retire_closed_session_refusal(42) && manager.cancellations_pending(),
              "closed-session proof ignored an unresolved native Add intent");
    }
}
template <class Fixture> void cancel_session_retry_regression() {
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    for (const bool bulk : {false, true}) {
        Fixture fixture;
        fixture.config.sole_instance = true;
        auto manager = fixture.manager();
        check(manager.place({.client_order_id = "clearing", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "clearing retry seed refused");
        fixture.poll(manager, 0);
        manager.on_reply(fixture.sent.back().id, {.msgid = 179, .order_id = 98001}, OrderManager::Clock::time_point{});
        fixture.ready = false;
        check((bulk ? manager.cancel_all(42) : manager.cancel("clearing")).empty(), "clearing cancel refused");
        fixture.poll(manager, 0);
        check(fixture.sent.size() == 2, "initial risk-reducing cancel was gated on Add readiness");
        for (int i = 0; i < 5; ++i) {
            const auto code = i % 3 == 0 ? 3 : i % 3 == 1 ? 4 : 66;
            manager.on_reply(fixture.sent.back().id,
                             {.msgid = bulk ? 186 : 177, .code = code, .message = "trading halted"},
                             OrderManager::Clock::time_point{} + std::chrono::milliseconds(i * 10000));
            fixture.ready = false;
            const auto before = fixture.sent.size();
            fixture.poll(manager, (i + 1) * 10000 - 1);
            check(fixture.sent.size() == before && !manager.operator_action_required(),
                  "clearing cancellation retried while suspended or spent its business budget");
            fixture.ready = true;
            fixture.poll(manager, (i + 1) * 10000);
            check(fixture.sent.size() == before + 1 && !manager.operator_action_required(),
                  "clearing cancellation did not resume after readiness or exhausted three business attempts");
        }
        manager.on_reply(fixture.sent.back().id, {.msgid = bulk ? 186 : 177, .num_orders = 1},
                         OrderManager::Clock::time_point{} + std::chrono::milliseconds(50000), 1);
        plaza2::private_state::OwnOrderSnapshot row;
        row.public_order_id = row.private_order_id = 98001;
        row.sess_id = 100;
        row.isin_id = 42;
        row.client_code = "ABCD001";
        row.dir = 1;
        row.public_amount = 2;
        row.public_action = 0;
        row.from_trade_repl = true;
        row.trade_repl_commit_sequence = 2;
        manager.observe_orders(std::span(&row, 1));
        check(manager.orders().at("clearing").state == OrderState::Cancelled && !manager.operator_action_required() &&
                  !manager.cancellations_pending(),
              "resumed clearing cancel did not settle from committed TRADE");
    }
}
} // namespace moex::connector_host
