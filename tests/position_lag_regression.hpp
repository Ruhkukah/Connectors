#pragma once

#include "moex/connector_host/order_manager.hpp"
#include <iostream>
#include <stdexcept>

namespace moex::connector_host::regression::position_lag {
namespace cg = plaza2::cgate;
namespace ps = plaza2::private_state;
namespace tr = plaza2_trade;
inline void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Fixture {
    OrderManagerConfig config;
    std::int64_t position{};
    std::optional<PositionProof> proof{PositionProof{.trade_lifenum = 7, .calendar_revision = 100}};
    std::vector<std::uint32_t> sent;
    std::vector<std::string> events;
    Fixture() {
        config.broker_code = "ABCD";
        config.client_code = "001";
        config.login_from = "owner-login";
        config.risk.max_position_by_isin = {{42, 3}};
    }
    OrderManager manager(bool with_proof = true) {
        return OrderManager(
            config,
            [this](const auto&, auto id) {
                sent.push_back(id);
                return cg::Plaza2PublisherMessageResult{.certainty = cg::Plaza2SubmissionCertainty::Posted,
                                                        .post_invoked = true};
            },
            [](auto) { return true; },
            [](auto isin) -> std::optional<ps::FutureSessionTerms> {
                ps::FutureSessionTerms terms;
                terms.isin_id = isin;
                terms.sess_id = 100;
                terms.min_step = ps::SessionDecimal{100000};
                terms.bounds.interval_valid = true;
                return terms;
            },
            [this](auto kind, auto) { events.emplace_back(kind); },
            [this](auto) -> std::optional<std::int64_t> { return position; },
            with_proof ? OrderManager::PositionAnchor([this](auto) { return proof; }) : OrderManager::PositionAnchor{});
    }
};
inline OrderRequest request(std::string key, std::int32_t quantity,
                            tr::Plaza2TradeSide side = tr::Plaza2TradeSide::Buy) {
    return {.client_order_id = std::move(key), .isin_id = 42, .side = side, .price = "100", .quantity = quantity};
}
inline ps::OwnTradeSnapshot fill(std::int64_t id, std::int64_t quantity = 2) {
    ps::OwnTradeSnapshot row;
    row.id_deal = id;
    row.sess_id = 100;
    row.isin_id = 42;
    row.amount = quantity;
    row.code_buy = "ABCD001";
    row.code_sell = "OTHER01";
    row.login_buy = "another-own-login";
    row.private_order_id_buy = id + 10000;
    row.private_order_id_sell = id + 20000;
    row.price = "100";
    row.repl_rev = 101;
    row.trade_lifenum = 7;
    return row;
}
inline void observe(OrderManager& manager, const ps::OwnTradeSnapshot& row) {
    manager.observe_trades(std::span(&row, 1));
}
inline void run() {
    for (const bool with_proof : {false, true}) {
        Fixture f;
        auto manager = f.manager(with_proof);
        const auto row = fill(77001);
        observe(manager, row);
        observe(manager, row);
        check(!manager.place(request("lagged-pos", 2)).empty(),
              "own TRADE fill was omitted from position risk while POS lagged");
        check(manager.place(request("exact-headroom", 1)).empty(), "own fill replay reserved its quantity twice");
    }
    {
        Fixture f;
        auto manager = f.manager();
        auto foreign = fill(77002);
        foreign.code_buy = "OTHER01";
        observe(manager, foreign);
        check(manager.place(request("foreign-fill", 3)).empty(), "foreign account fill consumed own position risk");
        f.proof.reset();
        check(!manager.place(request("missing-proof", 1)).empty(), "missing native POS fill proof assumed catch-up");
    }
    {
        Fixture f;
        auto manager = f.manager();
        check(manager.place(request("late-identity", 2)).empty(), "position-lag Add seed refused");
        manager.poll(OrderManager::Clock::time_point{}, 1700000000);
        auto row = fill(77003);
        observe(manager, row); // Unknown order IDs still reserve the physical fill.
        manager.on_reply(f.sent.front(), {.msgid = 179, .order_id = row.private_order_id_buy},
                         OrderManager::Clock::time_point{});
        observe(manager, row);
        check(manager.orders().at("late-identity").executed == 2, "deferred fill was lost or credited twice");
        ps::OwnOrderSnapshot terminal;
        terminal.public_order_id = terminal.private_order_id = row.private_order_id_buy;
        terminal.sess_id = 100;
        terminal.isin_id = 42;
        terminal.client_code = "ABCD001";
        terminal.login_from = "owner-login";
        terminal.dir = 1;
        terminal.price = "100";
        terminal.public_amount = 2;
        terminal.public_action = 2;
        terminal.from_trade_repl = true;
        manager.observe_orders(std::span(&terminal, 1));
        check(!manager.place(request("terminal-fill-lag", 2)).empty(),
              "terminal order released its own filled position before POS caught up");
        f.position = 2;
        f.proof->last_deal_id = 999999;
        f.proof->bought = 2;
        check(!manager.place(request("arbitrary-marker", 1)).empty(),
              "larger arbitrary POS deal ID released fill risk");
        f.proof->last_deal_id = row.id_deal;
        f.proof->bought = 1;
        check(!manager.place(request("incomplete-counter", 1)).empty(),
              "exact marker ignored insufficient POS quantity");
        f.proof->bought = 2;
        check(manager.place(request("caught-up", 1)).empty(), "exact committed POS proof did not release fill reserve");
        check(manager.cancel("caught-up").empty(), "unsent caught-up fixture cancel");
        f.position = 1;
        f.proof->bought = 1;
        check(!manager.place(request("regressed-pos", 1)).empty(), "regressed same-calendar POS reused prior coverage");
        f.position = 2;
        f.proof->bought = 2;
        row.trade_lifenum = 8;
        row.repl_rev = 201;
        observe(manager, row);
        check(manager.orders().at("late-identity").executed == 2, "new-epoch replay credited the same deal again");
    }
    {
        Fixture f;
        f.position = 2;
        f.proof->last_deal_id = 77004;
        f.proof->bought = 2;
        auto manager = f.manager();
        const auto row = fill(77004);
        observe(manager, row);
        check(manager.place(request("pos-ahead", 1)).empty(), "POS ahead of TRADE was reserved a second time");
    }
    {
        Fixture f;
        f.config.risk.max_position_by_isin[42] = 5;
        auto manager = f.manager();
        auto first = fill(77005);
        auto second = fill(77006, 1);
        second.repl_rev = 102;
        observe(manager, first);
        observe(manager, second);
        f.position = 3;
        f.proof->last_deal_id = second.id_deal;
        f.proof->bought = 2;
        check(!manager.place(request("batch-incomplete", 1)).empty(),
              "batch marker ignored an earlier unproved own fill");
        f.proof->bought = 3;
        check(manager.place(request("batch-complete", 2)).empty(),
              "exact batch marker did not reconcile its fill prefix");
    }
    {
        Fixture f;
        auto manager = f.manager();
        auto self = fill(77007);
        self.code_sell = "ABCD001";
        observe(manager, self);
        check(!manager.place(request("self-buy-lag", 2)).empty() &&
                  !manager.place(request("self-sell-lag", 2, tr::Plaza2TradeSide::Sell)).empty(),
              "self-matched buy/sell fills netted away pending position risk");
        f.proof->last_deal_id = self.id_deal;
        f.proof->bought = 2;
        check(!manager.place(request("self-half-proof", 2)).empty(),
              "buy counter alone released self-matched sell reserve");
        f.proof->sold = 2;
        check(manager.place(request("self-complete", 3)).empty(),
              "both POS counters did not prove a self-matched fill");
    }
    {
        Fixture f;
        auto manager = f.manager();
        auto row = fill(77008);
        observe(manager, row);
        f.proof->calendar_revision = 200;
        check(!manager.place(request("info-only", 2)).empty(),
              "POS.info advancement alone discarded a fill reservation");
        f.position = 2;
        f.proof->bought = f.proof->day_open_bought = 2;
        check(manager.place(request("calendar-complete", 1)).empty(),
              "committed day-open quantity did not cover calendar prefix");
        check(manager.cancel("calendar-complete").empty(), "unsent calendar fixture cancel");
        f.position = 0;
        f.proof->day_open_bought = 1;
        check(!manager.place(request("day-open-regressed", 1)).empty(),
              "regressed day-open proof reused prior fill coverage");
    }
    {
        Fixture f;
        f.config.risk.max_position_by_isin[42] = 4;
        auto manager = f.manager();
        auto before_calendar = fill(77011);
        auto after_calendar = fill(77012, 1);
        after_calendar.repl_rev = 201;
        observe(manager, before_calendar);
        observe(manager, after_calendar);
        f.position = 3;
        f.proof->calendar_revision = 200;
        f.proof->last_deal_id = after_calendar.id_deal;
        f.proof->bought = 3;
        check(!manager.place(request("calendar-bypass", 1)).empty(),
              "later exact marker bypassed missing day-open proof for calendar fills");
        f.proof->day_open_bought = 2;
        check(manager.place(request("calendar-and-marker", 1)).empty(),
              "day-open and exact marker proofs did not reconcile their separate prefixes");
    }
    {
        Fixture f;
        auto manager = f.manager();
        auto first = fill(77013, 1);
        auto second = fill(77014, 1);
        second.repl_rev = 102;
        observe(manager, first);
        observe(manager, second);
        f.position = 2;
        f.proof->bought = 2;
        f.proof->last_deal_id = second.id_deal;
        check(manager.place(request("known-marker", 1)).empty(), "known marker proof refused");
        check(manager.cancel("known-marker").empty(), "unsent known-marker fixture cancel");
        f.proof->last_deal_id = 999999;
        check(manager.place(request("unknown-marker", 1)).empty(), "unknown marker erased valid covered quantities");
        check(manager.cancel("unknown-marker").empty(), "unsent unknown-marker fixture cancel");
        f.proof->last_deal_id = first.id_deal;
        check(!manager.place(request("older-after-unknown", 1)).empty(),
              "intervening unknown POS marker forgot the last validated source revision");
    }
    {
        Fixture f;
        auto manager = f.manager();
        auto row = fill(77009);
        observe(manager, row);
        f.position = 2;
        f.proof->trade_lifenum = 8;
        f.proof->calendar_revision = 200;
        f.proof->last_deal_id = row.id_deal;
        f.proof->bought = 2;
        check(!manager.place(request("wrong-epoch", 1)).empty(), "cross-epoch POS marker released an own fill");
        row.trade_lifenum = 8;
        row.repl_rev = 201;
        observe(manager, row);
        check(manager.place(request("replayed-epoch", 1)).empty(),
              "committed new-epoch replay did not repair fill provenance");
    }
    {
        Fixture f;
        auto manager = f.manager();
        check(manager.place(request("source-conflict", 2)).empty(), "conflict fixture Add refused");
        manager.poll(OrderManager::Clock::time_point{}, 1700000000);
        auto row = fill(77010);
        observe(manager, row);
        row.amount = 1;
        observe(manager, row);
        manager.on_reply(f.sent.front(), {.msgid = 179, .order_id = row.private_order_id_buy},
                         OrderManager::Clock::time_point{});
        observe(manager, row);
        const auto& order = manager.orders().at("source-conflict");
        check(order.executed == 0 && order.state == OrderState::Unknown && order.operator_action_required &&
                  std::count(f.events.begin(), f.events.end(), "trade_source_conflict") == 1,
              "contradictory unmatched deal quantity was credited after logical identity appeared");
        check(!manager.place(request("conflicted-entry", 1)).empty(), "conflicting own fill source allowed new risk");
    }
    {
        Fixture f;
        f.config.risk.max_position_by_isin[42] = 200000;
        auto manager = f.manager();
        std::vector<ps::OwnTradeSnapshot> rows(150000, fill(100000, 1));
        for (std::size_t i = 0; i < rows.size(); ++i) {
            rows[i].id_deal += i;
            rows[i].private_order_id_buy += i;
            rows[i].repl_rev += i;
        }
        manager.observe_trades(rows);
        check(manager.place(request("indexed-lag-first", 1)).empty(),
              "indexed fill-risk fixture refused valid headroom");
        auto later = fill(999999, 1);
        later.repl_rev = 999999;
        observe(manager, later);
        std::int64_t slowest{};
        for (int i = 0; i < 8; ++i) {
            const auto start = OrderManager::Clock::now();
            check(manager.place(request("indexed-lag-" + std::to_string(i), 1)).empty(),
                  "indexed lagged-fill headroom refused");
            slowest = std::max(
                slowest,
                std::chrono::duration_cast<std::chrono::microseconds>(OrderManager::Clock::now() - start).count());
        }
        check(slowest < 1000, "position admission scanned pending/history fills after a new later trade");
        std::cout << "150k indexed pending-fill admission max_us=" << slowest << '\n';
    }
}
} // namespace moex::connector_host::regression::position_lag

namespace moex::connector_host {
inline void position_lag_regression() {
    regression::position_lag::run();
}
} // namespace moex::connector_host
