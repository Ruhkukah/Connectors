#pragma once

#include "moex/connector_host/order_manager.hpp"

#include <algorithm>
#include <stdexcept>

namespace moex::connector_host {
template <class Fixture> void retired_command_regression() {
    namespace ps = plaza2::private_state;
    const auto check = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    const auto time = [](std::int64_t ms) { return OrderManager::Clock::time_point{} + std::chrono::milliseconds(ms); };
    const auto logged = [](const Fixture& fixture, std::string_view kind, std::uint32_t id) {
        return std::find(fixture.log.begin(), fixture.log.end(),
                         std::string(kind) + "{\"user_id\":" + std::to_string(id) + "}") != fixture.log.end();
    };
    {
        Fixture fixture;
        auto manager = fixture.manager();
        check(manager.place({.client_order_id = "retired-moves", .isin_id = 42, .price = "100", .quantity = 2}).empty(),
              "retired Move seed refused");
        fixture.poll(manager, 0);
        manager.on_reply(fixture.sent.front().id, {.msgid = 179, .order_id = 99342}, time(0));
        ps::OwnOrderSnapshot live;
        live.public_order_id = live.private_order_id = 99342;
        live.sess_id = 100;
        live.isin_id = 42;
        live.client_code = "ABCD001";
        live.login_from = "owner-login";
        live.price = "100";
        live.public_amount = live.public_amount_rest = 2;
        live.public_action = live.dir = 1;
        live.ext_id = manager.orders().at("retired-moves").ext_id;
        live.from_trade_repl = true;
        manager.observe_orders(std::span(&live, 1));
        for (int iteration = 0; iteration < 64; ++iteration) {
            const auto ms = 1000LL * (iteration + 1);
            check(manager.move("retired-moves", "101", 2).empty(), "retired Move retained a blocking correlation");
            fixture.poll(manager, ms);
            const auto id = fixture.sent.back().id;
            manager.on_timeout(id, time(ms + 1));
            manager.prove_absence(1700000000 + ms / 1000 + 61, true);
            manager.on_timeout(id, time(ms + 2));
            check(logged(fixture, "late_timeout", id), "proved-unapplied Move retained its pending UID");
            manager.on_reply(id, {.msgid = 176, .order_id1 = 99999}, time(ms + 2));
            check(logged(fixture, "unknown_reply", id) && manager.orders().at("retired-moves").order_id == 99342 &&
                      manager.orders().at("retired-moves").state == OrderState::Working &&
                      !manager.operator_action_required(),
                  "retired Move reply restored correlation or changed authoritative old identity");
        }
        check(fixture.sent.size() == 65, "retired Move histories caused retries or cancellation");
    }
    {
        Fixture fixture;
        fixture.config.reply_timeout = std::chrono::milliseconds(100);
        fixture.config.sole_instance = true;
        auto manager = fixture.manager();
        for (int renewal = 0; renewal < 32; ++renewal) {
            check(manager.cancel_all(42).empty(), "explicit DUO renewal refused");
            const auto start = 10000LL * renewal;
            fixture.poll(manager, start);
            for (int attempt = 0; attempt < 3; ++attempt) {
                const auto ms = start + attempt * 1100 + 100;
                manager.on_timeout(fixture.sent.back().id, time(ms));
                fixture.poll(manager, ms + 1000);
            }
            const auto id = fixture.sent.back().id;
            manager.on_timeout(id, time(start + 4000));
            check(logged(fixture, "late_timeout", id), "exhausted DUO retained its pending UID");
            manager.on_reply(id, {.msgid = 186, .num_orders = 0}, time(start + 4000), renewal * 2 + 1);
            manager.observe_trade_commit(renewal * 2 + 2);
            check(
                logged(fixture, "unknown_reply", id) && manager.operator_action_required() &&
                    manager.cancellations_pending() &&
                    !manager.place({.client_order_id = "unsafe", .isin_id = 42, .price = "100", .quantity = 1}).empty(),
                "retired exhausted DUO reply cleared unresolved broad intent");
        }
        check(fixture.sent.size() == 96, "exhausted DUO histories caused additional automatic posts");
    }
}
} // namespace moex::connector_host
