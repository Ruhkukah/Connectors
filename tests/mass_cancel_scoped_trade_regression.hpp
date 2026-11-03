#pragma once
#include "moex/connector_host/order_manager.hpp"
#include "fixtures/cgate99_messages.hpp"
#include <cstring>
#include <array>
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void mass_cancel_scoped_trade_regression() {
    namespace ps = plaza2::private_state;
    namespace tr = plaza2_trade;
    const auto check = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    const auto native = [](std::int64_t id, std::int32_t isin, std::uint64_t commit) {
        ps::OwnOrderSnapshot row;
        row.public_order_id = row.private_order_id = id;
        row.sess_id = 100;
        row.isin_id = isin;
        row.client_code = "ABCD001";
        row.login_from = "owner-login";
        row.price = "100";
        row.public_amount = row.public_amount_rest = 3;
        row.ext_id = static_cast<std::int32_t>(id);
        row.dir = row.public_action = 1;
        row.from_trade_repl = true;
        row.trade_repl_commit_sequence = commit;
        return row;
    };
    Fixture fixture;
    fixture.config.sole_instance = true;
    auto manager = fixture.manager();
    const std::array initial{native(1201, 42, 1), native(1202, 42, 1), native(1301, 43, 1)};
    manager.observe_orders(initial, true);
    check(manager.cancel_all(42).empty(), "scoped bulk cancellation refused");
    fixture.poll(manager, 0);
    check(fixture.sent.size() == 1 && fixture.sent.front().kind == tr::Plaza2TradeCommandKind::DelUserOrders,
          "scoped bulk command missing");
    manager.on_reply(fixture.sent.front().id, {.msgid = 186, .num_orders = 2}, OrderManager::Clock::time_point{}, 5);
    const auto held = [&] {
        fixture.poll(manager, 1000);
        check(fixture.sent.size() == 1 && manager.queued() == 0 && manager.cancellations_pending(42),
              "unrelated or incomplete TRADE evidence released per-ID fallbacks");
        check(!manager.place({.client_order_id = "still-gated", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
              "unproved bulk scope admitted new entry");
    };
    manager.observe_trade_commit(6); // Empty commit/heartbeat is not identity evidence.
    held();
    auto unrelated = native(1301, 43, 7);
    manager.observe_orders(std::span(&unrelated, 1));
    held();
    auto wrong = native(1201, 42, 8);
    wrong.client_code = "OTHER";
    manager.observe_orders(std::span(&wrong, 1));
    held();
    wrong = native(1201, 43, 9); // The numeric ID alone does not establish the scope.
    manager.observe_orders(std::span(&wrong, 1));
    held();
    wrong = native(1201, 42, 10);
    wrong.sess_id = 99;
    manager.observe_orders(std::span(&wrong, 1));
    held();
    auto cached = native(1201, 42, 5);
    manager.observe_orders(std::span(&cached, 1)); // Already included in the reply barrier.
    held();
    auto user_book = native(1201, 42, 11);
    user_book.from_trade_repl = false;
    user_book.from_user_book = true;
    user_book.private_amount = user_book.private_amount_rest = 3;
    user_book.private_action = 1;
    manager.observe_orders(std::span(&user_book, 1), true);
    manager.observe_trade_commit(11);
    held();
    auto first = native(1201, 42, 12);
    manager.observe_orders(std::span(&first, 1));
    held(); // One refreshed identity cannot release the other stale survivor.
    first.public_amount_rest = first.public_action = 0;
    first.trade_repl_commit_sequence = 13;
    manager.observe_orders(std::span(&first, 1));
    held(); // Terminal first order needs no fallback, but second still lacks proof.
    auto survivor = native(1202, 42, 14);
    manager.observe_orders(std::span(&survivor, 1));
    fixture.poll(manager, 1001);
    check(fixture.sent.size() == 2 && fixture.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder &&
              manager.orders().at("recovered:100:1201").state == OrderState::Cancelled,
          "exact postbarrier survivor proof did not enable only its per-ID fallback");
    check(
        manager.cancellations_pending(42) &&
            !manager
                 .place({.client_order_id = "fallback-is-not-completion", .isin_id = 42, .price = "100", .quantity = 1})
                 .empty(),
        "working-row proof or queued fallback was treated as bulk completion");
    official_cgate99::DelOrder payload{};
    check(fixture.sent.back().payload.size() == sizeof(payload), "fallback wire size changed");
    std::memcpy(&payload, fixture.sent.back().payload.data(), sizeof(payload));
    check(payload.order_id == 1202, "fallback targeted a terminal or unrelated identity");
    const auto time = [](std::int64_t ms) { return OrderManager::Clock::time_point{} + std::chrono::milliseconds(ms); };
    manager.on_reply(fixture.sent.back().id, {.msgid = 99, .penalty_remain = 1000}, time(1001));
    fixture.poll(manager, 2000);
    check(fixture.sent.size() == 2, "scoped fallback ignored flood penalty");
    fixture.poll(manager, 2001);
    check(fixture.sent.size() == 3 && fixture.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder,
          "retained bulk gate discarded the fallback flood retry");
    manager.on_reply(fixture.sent.back().id, {.msgid = 177, .code = 1}, time(2001));
    fixture.poll(manager, 3001);
    check(fixture.sent.size() == 4 && fixture.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder,
          "retained bulk gate discarded the fallback business-rejection retry");
    manager.on_reply(fixture.sent.back().id, {.msgid = 177}, time(3001));
    manager.observe_trade_commit(15);
    check(manager.cancellations_pending(42), "cancel acknowledgement/empty commit proved terminal state");
    survivor.public_amount_rest = survivor.public_action = 0;
    survivor.trade_repl_commit_sequence = 16;
    manager.observe_orders(std::span(&survivor, 1));
    fixture.poll(manager, 3002);
    check(!manager.cancellations_pending(42) && !manager.operator_action_required() && fixture.sent.size() == 4,
          "native terminal proof did not retire the scope or resent cancellation");
    check(manager.place({.client_order_id = "after-terminal-scope", .isin_id = 42, .price = "100", .quantity = 1})
              .empty(),
          "terminal bulk scope left entry permanently gated");
    for (const bool relisted : {false, true}) {
        Fixture later;
        later.config.sole_instance = true;
        auto scoped = later.manager();
        auto original = native(2201, 42, 1);
        scoped.observe_orders(std::span(&original, 1), true);
        check(scoped.cancel_all(42).empty(), "later-identity bulk fixture refused");
        later.poll(scoped, 0);
        scoped.on_reply(later.sent.front().id, {.msgid = 186, .num_orders = 1}, time(0), 5);
        original.trade_repl_commit_sequence = 6;
        scoped.observe_orders(std::span(&original, 1));
        later.poll(scoped, 1001);
        check(later.sent.size() == 2, "first proved fallback did not become enabled");
        auto discovered = native(2202, 42, 7);
        discovered.from_trade_repl = false;
        discovered.from_user_book = true;
        discovered.private_amount = discovered.private_amount_rest = 3;
        discovered.private_action = 1;
        if (relisted) {
            discovered.sess_id = 101;
            discovered.id_ord1 = 2201;
            discovered.ext_id = original.ext_id;
        }
        scoped.observe_orders(std::span(&discovered, 1), true);
        const auto key = relisted ? "recovered:100:2201" : "recovered:100:2202";
        check(scoped.cancel(key).empty(), "later owned identity cancellation intent refused");
        scoped.observe_trade_commit(7);
        later.poll(scoped, 1002);
        check(later.sent.size() == 2 && scoped.queued() == 0,
              "enabled bulk scope leaked fallback permission to a USERORDERBOOK-only identity");
        scoped.on_reply(later.sent.back().id, {.msgid = 99, .penalty_remain = 1000}, time(1002));
        later.poll(scoped, 2002);
        check(later.sent.size() == 3, "later unproved identity suppressed an already proved target's retry");
        std::memcpy(&payload, later.sent.back().payload.data(), sizeof(payload));
        check(payload.order_id == 2201, "fallback retry inherited the unproved current identity");
        scoped.on_reply(later.sent.back().id, {.msgid = 177}, time(2002));
        discovered.from_trade_repl = true;
        discovered.from_user_book = false;
        discovered.trade_repl_commit_sequence = 8;
        scoped.observe_orders(std::span(&discovered, 1));
        later.poll(scoped, 2003);
        check(later.sent.size() == 4 && later.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder,
              "later exact TRADE witness did not release its own fallback");
        std::memcpy(&payload, later.sent.back().payload.data(), sizeof(payload));
        check(payload.order_id == 2202, "later witness targeted the previous identity");
        original.public_amount_rest = original.public_action = 0;
        original.trade_repl_commit_sequence = 9;
        discovered.public_amount_rest = discovered.public_action = 0;
        discovered.trade_repl_commit_sequence = 9;
        const std::array completed{original, discovered};
        scoped.observe_orders(completed);
        later.poll(scoped, 2004);
        check(!scoped.cancellations_pending(42) && later.sent.size() == 4,
              "later-identity terminal cleanup retained or resent fallback intent");
    }
}
} // namespace moex::connector_host
