#pragma once

#include "fixtures/cgate99_messages.hpp"
#include "moex/connector_host/order_manager.hpp"

#include <cstring>
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void mismatched_recovery_cancel_regression() {
    namespace ps = plaza2::private_state;
    namespace tr = plaza2_trade;
    const auto check = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    const auto duo_ext = [&](const auto& sent) {
        official_cgate99::DelUserOrders wire{};
        check(sent.payload.size() == sizeof(wire), "recovery DUO wire size mismatch");
        std::memcpy(&wire, sent.payload.data(), sizeof(wire));
        return wire.ext_id;
    };
    // The identity can be disproved while recovery cancellation is queued or
    // after it has crossed the transport boundary. Neither permits another send.
    for (const bool posted : {false, true})
        for (const auto late_msgid : {99, 100, 186}) {
            Fixture fixture;
            fixture.config.max_commands_per_second = 30;
            fixture.config.reply_timeout = std::chrono::milliseconds(100);
            auto manager = fixture.manager();
            for (const auto [key, isin] : {std::pair{"collision", 42}, {"unrelated", 43}, {"known", 44}})
                check(manager.place({.client_order_id = key, .isin_id = isin, .price = "100", .quantity = 2}).empty(),
                      "recovery isolation Add refused");
            fixture.poll(manager, 0);
            check(fixture.sent.size() == 3, "recovery isolation Adds did not post");
            const auto collision_add = fixture.sent[0].id;
            const auto unrelated_add = fixture.sent[1].id;
            manager.on_reply(fixture.sent[2].id, {.msgid = 179, .order_id = 98044}, OrderManager::Clock::time_point{});
            check(manager.cancel("known").empty(), "known-ID cancellation refused");
            const auto now = OrderManager::Clock::time_point{} + std::chrono::milliseconds(100);
            manager.on_timeout(collision_add, now);
            manager.on_timeout(unrelated_add, now);
            const auto collision_ext = manager.orders().at("collision").ext_id;
            const auto unrelated_ext = manager.orders().at("unrelated").ext_id;
            if (posted)
                fixture.poll(manager, 100);
            std::uint32_t collision_duo{};
            for (const auto& sent : fixture.sent)
                if (sent.kind == tr::Plaza2TradeCommandKind::DelUserOrders && duo_ext(sent) == collision_ext)
                    collision_duo = sent.id;
            check((collision_duo != 0) == posted, "recovery DUO was not in the requested transport state");

            ps::OwnOrderSnapshot foreign;
            foreign.public_order_id = foreign.private_order_id = 98042;
            foreign.sess_id = 100;
            foreign.isin_id = 42;
            foreign.client_code = "ABCD001";
            foreign.login_from = "another-instance";
            foreign.price = "100";
            foreign.public_amount = foreign.public_amount_rest = 2;
            foreign.public_action = foreign.dir = 1;
            foreign.ext_id = collision_ext;
            foreign.from_trade_repl = true;
            manager.observe_orders(std::span(&foreign, 1));
            const auto& collision = manager.orders().at("collision");
            check(collision.order_id == 0 && collision.state == OrderState::Unknown &&
                      collision.operator_action_required,
                  "mismatched recovery identity did not remain unresolved");
            const auto queued_before_cancel = manager.queued();
            const auto cancel_requested_before = collision.cancel_requested;
            const auto remaining_before = collision.remaining;
            check(manager.cancel("collision") == "identity conflict: resolve via broker/exchange, order_id unknown",
                  "explicit cancel accepted a conflicting ext-ID identity");
            check(manager.queued() == queued_before_cancel && collision.cancel_requested == cancel_requested_before &&
                      collision.operator_action_required && collision.remaining == remaining_before,
                  "refused conflict cancel mutated cancellation intent or exposure");
            if (posted) {
                manager.on_reply(collision_duo, {.msgid = late_msgid, .penalty_remain = 1, .num_orders = 0}, now);
                manager.on_timeout(collision_duo, now);
            }
            fixture.poll(manager, 101);
            const auto count_ext = [&](std::int32_t ext) {
                return std::count_if(fixture.sent.begin(), fixture.sent.end(), [&](const auto& sent) {
                    return sent.kind == tr::Plaza2TradeCommandKind::DelUserOrders && duo_ext(sent) == ext;
                });
            };
            check(count_ext(collision_ext) == (posted ? 1 : 0),
                  "late reply resumed a mismatched ext-ID recovery cancellation");
            check(count_ext(unrelated_ext) == 1, "mismatch purged another order's recovery intent");
            const auto known_cancel = std::find_if(fixture.sent.begin(), fixture.sent.end(), [](const auto& sent) {
                return sent.kind == tr::Plaza2TradeCommandKind::DelOrder;
            });
            check(known_cancel != fixture.sent.end(), "mismatch purged a known-ID cancellation");
            manager.on_reply(known_cancel->id, {.msgid = 99, .penalty_remain = 1}, now);
            for (const auto time : {102, 1000, 5000, 100000})
                fixture.poll(manager, time);
            check(count_ext(collision_ext) == (posted ? 1 : 0),
                  "pending timeout retried a mismatched ext-ID recovery cancellation");
            check(std::count_if(fixture.sent.begin(), fixture.sent.end(),
                                [](const auto& sent) { return sent.kind == tr::Plaza2TradeCommandKind::DelOrder; }) > 1,
                  "mismatch prevented known-ID risk reduction from retrying");
            manager.prove_absence(1700001000, true);
            check(manager.orders().at("collision").state == OrderState::Unknown &&
                      manager.orders().at("collision").operator_action_required,
                  "late recovery reply supplied false absence proof for a mismatched identity");
        }
}
} // namespace moex::connector_host
