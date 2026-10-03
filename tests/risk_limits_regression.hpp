#pragma once
#include "moex/connector_host/order_manager.hpp"
#include <climits>
#include <iostream>
#include <stdexcept>
namespace moex::connector_host::regression::risk_limits {
using namespace moex::connector_host;
namespace tr = moex::plaza2_trade;
namespace ps = moex::plaza2::private_state;
namespace cg = moex::plaza2::cgate;
inline void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Fixture {
    OrderManagerConfig config;
    std::map<int, std::optional<std::int64_t>> pos;
    std::vector<std::uint32_t> sent;
    Fixture() {
        config.broker_code = "ABCD";
        config.client_code = "001";
        config.risk.max_notional_scaled = INT64_MAX;
        config.max_commands_per_second = 30;
    }
    OrderManager manager() {
        return OrderManager(
            config,
            [&](const auto&, auto id) {
                sent.push_back(id);
                return cg::Plaza2PublisherMessageResult{.certainty = cg::Plaza2SubmissionCertainty::Posted,
                                                        .post_invoked = true};
            },
            [](auto) { return true; },
            [](auto isin) {
                ps::FutureSessionTerms t;
                t.isin_id = isin;
                t.sess_id = 100;
                t.min_step = ps::SessionDecimal{1000};
                t.bounds.interval_valid = true;
                t.bounds.lower = ps::SessionDecimal{0};
                t.bounds.upper = ps::SessionDecimal{INT64_MAX};
                return std::optional(t);
            },
            {},
            [&](auto isin) -> std::optional<std::int64_t> {
                return pos.contains(isin) ? pos.at(isin) : std::optional<std::int64_t>{0};
            });
    }
    void poll(OrderManager& m) {
        m.poll(OrderManager::Clock::time_point{} + std::chrono::seconds(1), 1700000000);
    }
};
inline OrderRequest add(std::string id, int isin, std::string price, int quantity = 1,
                        tr::Plaza2TradeSide side = tr::Plaza2TradeSide::Buy) {
    return {.client_order_id = std::move(id),
            .isin_id = isin,
            .side = side,
            .price = std::move(price),
            .quantity = quantity};
}
inline void run() {
    Fixture caps;
    caps.config.risk.max_notional_by_isin = {{42, 20 * 100000LL}, {43, 8000 * 100000LL}};
    auto limited = caps.manager();
    require(!limited.place(add("unconfigured", 44, "12")).empty(),
            "configured per-ISIN policy admitted an uncovered instrument");
    require(!limited.place(add("cny-over", 42, "12", 2)).empty(), "per-ISIN CNY quote cap was ignored");
    require(limited.place(add("alrs-at", 43, "4000", 2)).empty(), "ALRS cap inherited another contract's quote units");
    require(limited.place(add("cny-at", 42, "12")).empty(), "independent CNY cap was charged ALRS quote points");
    require(!limited.place(add("alrs-over", 43, "4000")).empty(),
            "same-ISIN aggregate notional omitted working orders");
    Fixture long_side;
    long_side.config.risk.max_position_by_isin = {{42, 3}};
    long_side.pos[42] = 2;
    auto buys = long_side.manager();
    require(!buys.place(add("buy-two", 42, "100", 2)).empty(), "long POS+candidate exceeded max-position");
    require(buys.place(add("buy-one", 42, "100")).empty(), "one-contract long headroom rejected");
    require(buys.place(add("sell-three", 42, "100", 3, tr::Plaza2TradeSide::Sell)).empty(),
            "opposite side reducing POS was rejected");
    require(!buys.place(add("another-buy", 42, "100")).empty(), "opposite-side working orders netted away long risk");
    Fixture short_side;
    short_side.config.risk.max_position_by_isin = {{42, 3}};
    short_side.pos[42] = -2;
    auto sells = short_side.manager();
    require(!sells.place(add("sell-two", 42, "100", 2, tr::Plaza2TradeSide::Sell)).empty(),
            "short POS+candidate exceeded max-position");
    require(sells.place(add("sell-one", 42, "100", 1, tr::Plaza2TradeSide::Sell)).empty(),
            "one-contract short headroom rejected");
    require(!sells.place(add("another-sell", 42, "100", 1, tr::Plaza2TradeSide::Sell)).empty(),
            "queued same-side sell omitted from risk");
    Fixture unavailable;
    unavailable.config.risk.max_position_by_isin = {{42, 3}};
    unavailable.pos[42] = std::nullopt;
    auto missing = unavailable.manager();
    require(missing.place(add("missing-pos", 42, "100")).find("POS authority") != std::string::npos,
            "missing POS authority silently assumed flat");
    Fixture move;
    move.config.risk.max_position_by_isin = {{42, 3}};
    auto replacing = move.manager();
    require(replacing.place(add("moving", 42, "100", 2)).empty(), "Move seed refused");
    move.poll(replacing);
    replacing.on_reply(move.sent.front(), {.msgid = 179, .order_id = 91001}, OrderManager::Clock::time_point{});
    move.pos[42] = 1;
    require(!replacing.move("moving", "100", 3).empty(), "Move ignored POS plus replacement same-side amount");
    require(replacing.move("moving", "100", 2).empty(), "Move counted old and replacement exposure twice");
    Fixture queued;
    queued.config.risk.max_position_by_isin = {{42, 2}};
    queued.config.max_commands_per_second = 1;
    auto delayed = queued.manager();
    require(delayed.place(add("first", 42, "100")).empty() && delayed.place(add("held", 42, "100")).empty(),
            "queued position setup refused");
    queued.poll(delayed);
    require(queued.sent.size() == 1, "queued position fixture did not hold a command");
    queued.pos[42] = 2;
    delayed.poll(OrderManager::Clock::time_point{} + std::chrono::seconds(2), 1700000001);
    require(queued.sent.size() == 1 && delayed.orders().at("held").state == OrderState::Rejected,
            "rate-held Add ignored newly committed POS risk before sending");
    Fixture recovered;
    recovered.config.risk.max_position_by_isin = {{42, 2}};
    auto reconstructed = recovered.manager();
    ps::OwnOrderSnapshot own;
    own.public_order_id = own.private_order_id = 94001;
    own.sess_id = 100;
    own.isin_id = 42;
    own.client_code = "ABCD001";
    own.price = "100";
    own.dir = 1;
    own.public_amount = own.public_amount_rest = 2;
    own.public_action = 1;
    own.from_trade_repl = true;
    reconstructed.observe_orders(std::span(&own, 1), true);
    require(!reconstructed.place(add("after-rebuild", 42, "100")).empty(),
            "recovered same-side quantity was omitted from max-position");
    Fixture uncertain;
    uncertain.config.risk.max_position_by_isin = {{42, 6}};
    auto ambiguity = uncertain.manager();
    require(ambiguity.place(add("uncertain", 42, "100", 3)).empty(), "uncertain Move setup refused");
    uncertain.poll(ambiguity);
    ambiguity.on_reply(uncertain.sent.front(), {.msgid = 179, .order_id = 95001}, OrderManager::Clock::time_point{});
    require(ambiguity.move("uncertain", "100", 5).empty(), "uncertain Move reservation refused");
    uncertain.poll(ambiguity);
    ambiguity.on_timeout(uncertain.sent.back(), OrderManager::Clock::time_point{});
    require(!ambiguity.place(add("over-reservation", 42, "100", 2)).empty(),
            "timed-out Move replacement was omitted from position risk");
    Fixture large;
    large.config.risk.max_open_orders = 200000;
    large.config.risk.max_position_by_isin = {{42, 1000000}};
    auto indexed = large.manager();
    std::vector<ps::OwnOrderSnapshot> rows(150000, own);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        rows[i].public_order_id = rows[i].private_order_id = 100000 + i;
        rows[i].public_amount = rows[i].public_amount_rest = 1;
    }
    indexed.observe_orders(rows, true);
    std::int64_t slowest{};
    for (int i = 0; i < 8; ++i) {
        const auto start = OrderManager::Clock::now();
        require(indexed.place(add("indexed-" + std::to_string(i), 42, "100")).empty(),
                "indexed risk fixture rejected valid headroom");
        slowest = std::max(
            slowest, std::chrono::duration_cast<std::chrono::microseconds>(OrderManager::Clock::now() - start).count());
    }
    require(slowest < 1000, "position admission scanned 150k order history");
    std::cout << "150k indexed admission max_us=" << slowest << "\n";
    Fixture extremes;
    extremes.config.risk.max_position_by_isin = {{42, INT64_MAX}};
    extremes.pos[42] = INT64_MIN;
    auto reduce = extremes.manager();
    require(reduce.place(add("reduce-short", 42, "100")).empty(),
            "signed position magnitude overflow blocked risk reduction");
    require(!reduce.place(add("add-short", 42, "100", 1, tr::Plaza2TradeSide::Sell)).empty(),
            "INT64_MIN short POS escaped max-position");
    std::cout << "CRT11 quote/position regressions passed\n";
}
} // namespace moex::connector_host::regression::risk_limits
