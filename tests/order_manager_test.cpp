#include "moex/connector_host/order_manager.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace moex::connector_host;
namespace tr = moex::plaza2_trade;
namespace cg = moex::plaza2::cgate;
namespace ps = moex::plaza2::private_state;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Sent {
    tr::Plaza2TradeCommandKind kind;
    std::uint32_t id;
    std::int64_t ms;
};
struct Fixture {
    bool ready{true};
    std::int32_t session{100};
    std::int64_t ms{};
    cg::Plaza2SubmissionCertainty certainty{cg::Plaza2SubmissionCertainty::Posted};
    cg::Plaza2Error validation_error;
    std::vector<Sent> sent;
    std::vector<std::string> log;
    OrderManagerConfig config;
    Fixture() {
        config.broker_code = "ABCD";
        config.client_code = "001";
        config.max_commands_per_second = 5;
    }
    std::optional<ps::FutureSessionTerms> terms(std::int32_t isin) {
        ps::FutureSessionTerms terms;
        terms.isin_id = isin;
        terms.sess_id = session;
        terms.min_step = ps::SessionDecimal{100000};
        terms.bounds.lower = ps::SessionDecimal{100000};
        terms.bounds.upper = ps::SessionDecimal{100000000};
        terms.bounds.interval_valid = true;
        return terms;
    }
    OrderManager manager() {
        return OrderManager(
            config,
            [this](const auto& command, auto id) {
                sent.push_back({command.command_kind, id, ms});
                return cg::Plaza2PublisherMessageResult{
                    .certainty = certainty,
                    .validation_error = validation_error,
                    .post_invoked = certainty != cg::Plaza2SubmissionCertainty::DefinitelyNotSent};
            },
            [this](auto) { return ready; }, [this](auto isin) { return terms(isin); },
            [this](auto kind, auto fields) { log.push_back(std::string(kind) + std::string(fields)); });
    }
    void poll(OrderManager& manager, std::int64_t time) {
        ms = time;
        manager.poll(OrderManager::Clock::time_point{} + std::chrono::milliseconds(time), 1700000000 + time / 1000);
    }
};
OrderRequest request(std::string id, std::int32_t quantity = 3) {
    return {.client_order_id = std::move(id), .isin_id = 42, .price = "100", .quantity = quantity};
}
ps::OwnOrderSnapshot row(const ManagedOrder& order, std::int64_t id, std::int64_t remaining, std::int8_t action,
                         std::int32_t session = 100) {
    ps::OwnOrderSnapshot result;
    result.public_order_id = id;
    result.private_order_id = id;
    result.sess_id = session;
    result.isin_id = order.request.isin_id;
    result.client_code = "ABCD001";
    result.price = order.request.price;
    result.public_amount = order.request.quantity;
    result.public_amount_rest = remaining;
    result.ext_id = order.ext_id;
    result.dir = 1;
    result.public_action = action;
    result.from_trade_repl = true;
    return result;
}
void observe(OrderManager& manager, const ps::OwnOrderSnapshot& order, bool restart = false) {
    manager.observe_orders(std::span(&order, 1), restart);
}
void burst() {
    Fixture f;
    auto manager = f.manager();
    for (int i = 0; i < 50; ++i)
        require(manager.place(request("burst" + std::to_string(i))).empty(), "burst command refused");
    for (std::int64_t time = 0; time < 12000; time += 10)
        f.poll(manager, time);
    require(f.sent.size() == 50 && manager.queued() == 0, "throttled commands were dropped");
    for (const auto& sent : f.sent) {
        const auto count = std::count_if(f.sent.begin(), f.sent.end(), [&](const auto& other) {
            return other.ms >= sent.ms && other.ms < sent.ms + 1000;
        });
        require(count <= 5, "configured limit exceeded in a rolling second");
    }
}
void cancels() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("working")).empty(), "place refused");
    f.poll(manager, 0);
    const auto add_id = f.sent.front().id;
    manager.on_reply(add_id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    for (int i = 0; i < 10; ++i)
        require(manager.place(request("queued" + std::to_string(i))).empty(), "queued Add refused");
    require(manager.cancel("working").empty(), "cancel refused");
    f.poll(manager, 1);
    require(f.sent.at(1).kind == tr::Plaza2TradeCommandKind::DelOrder, "cancel did not overtake queued Adds");
    const auto first_cancel = f.sent.at(1).id;
    manager.on_reply(first_cancel, {.msgid = 99, .penalty_remain = 2000},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(1));
    const auto before_penalty = f.sent.size();
    f.poll(manager, 1001);
    require(f.sent.size() == before_penalty, "99 penalty ignored");
    f.poll(manager, 2001);
    require(f.sent.at(before_penalty).kind == tr::Plaza2TradeCommandKind::DelOrder, "cancel was dropped after 99");
    const auto second_cancel = f.sent.at(before_penalty).id;
    manager.on_reply(second_cancel, {.msgid = 177, .code = 17, .message = "fill race"},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(2001));
    f.poll(manager, 4000);
    require(f.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder ||
                std::any_of(f.sent.begin() + static_cast<std::ptrdiff_t>(before_penalty + 1), f.sent.end(),
                            [](auto v) { return v.kind == tr::Plaza2TradeCommandKind::DelOrder; }),
            "failed cancel did not retry");
    observe(manager, row(manager.orders().at("working"), 1001, 0, 0));
    require(manager.orders().at("working").state == OrderState::Cancelled,
            "exchange cancellation did not resolve failed cancel");
}
void ambiguity() {
    Fixture f;
    f.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
    auto manager = f.manager();
    require(manager.place(request("uncertain")).empty(), "ambiguous Add refused");
    f.poll(manager, 0);
    require(manager.orders().at("uncertain").state == OrderState::Unknown, "uncertain Add was assumed sent or absent");
    f.certainty = cg::Plaza2SubmissionCertainty::Posted;
    f.poll(manager, 1000);
    for (const auto& item : f.sent)
        if (item.kind == tr::Plaza2TradeCommandKind::DelUserOrders)
            manager.on_reply(item.id, {.msgid = 186, .num_orders = 0},
                             OrderManager::Clock::time_point{} + std::chrono::milliseconds(1000));
    manager.prove_absence(1700000050, true);
    require(manager.orders().at("uncertain").state == OrderState::Unknown,
            "absence accepted before bounded TRADE watermark");
    manager.prove_absence(1700000062, true);
    require(manager.orders().at("uncertain").state == OrderState::Cancelled, "absence proof did not resolve Add");
    f.poll(manager, 70000);
    require(std::count_if(f.sent.begin(), f.sent.end(),
                          [](auto item) { return item.kind == tr::Plaza2TradeCommandKind::AddOrder; }) == 1,
            "ambiguous Add was resent");
    manager.on_reply(999999, {.msgid = 179, .order_id = 999}, OrderManager::Clock::time_point{});
    require(manager.orders().at("uncertain").state == OrderState::Cancelled, "unknown reply altered order state");
}
void day_and_restart() {
    Fixture f;
    auto manager = f.manager();
    for (const auto id : {"a", "b", "c"})
        require(manager.place(request(id)).empty(), "concurrent Add refused");
    f.poll(manager, 0);
    std::vector<ps::OwnOrderSnapshot> rows;
    std::int64_t id = 1000;
    for (const auto& [key, order] : manager.orders())
        rows.push_back(row(order, ++id, 3, 1));
    manager.observe_orders(rows);
    observe(manager, row(manager.orders().at("a"), 1001, 2, 2));
    require(manager.orders().at("a").state == OrderState::PartFilled && manager.orders().at("a").executed == 1,
            "partial fill missing");
    f.ready = false;
    require(!manager.place(request("clearing")).empty(), "clearing allowed Add");
    require(manager.cancel("b").empty(), "clearing blocked cancel");
    f.poll(manager, 1000);
    observe(manager, row(manager.orders().at("b"), 1002, 0, 0));
    f.ready = true;
    f.session = 101;
    observe(manager, row(manager.orders().at("c"), 2003, 3, 1, 101));
    require(manager.orders().at("c").order_id == 2003 && manager.orders().at("c").sess_id == 101,
            "multi-day order rename missed");
    require(manager.move("c", "101", 2).empty(), "single-order MoveOrder rejected by codec");
    f.poll(manager, 2000);
    const auto moved = std::find_if(f.sent.rbegin(), f.sent.rend(),
                                    [](auto cmd) { return cmd.kind == tr::Plaza2TradeCommandKind::MoveOrder; });
    require(moved != f.sent.rend(), "MoveOrder not sent");
    manager.on_reply(moved->id, {.msgid = 176, .order_id1 = 3003},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(2000));
    require(manager.orders().at("c").order_id == 3003 && manager.orders().at("c").remaining == 2,
            "MoveOrder result not followed");
    observe(manager, row(manager.orders().at("a"), 1001, 0, 2));
    require(manager.place(request("evening", 2)).empty(), "fill permanently froze order entry");
    // Rebuild three working orders and a partial fill into an independent process.
    std::vector<ps::OwnOrderSnapshot> recovered;
    for (int i = 0; i < 3; ++i) {
        auto original = manager.orders().at("c");
        original.ext_id = 100 + i;
        auto value = row(original, 5000 + i, i == 0 ? 1 : 2, 1, 101);
        value.public_amount = 2;
        value.from_trade_repl = false;
        value.from_user_book = true;
        value.private_amount = 2;
        value.private_amount_rest = value.public_amount_rest;
        recovered.push_back(value);
    }
    auto restart = f.manager();
    restart.observe_orders(recovered, true);
    require(restart.orders().size() == 3, "restart lost working orders");
    require(restart.orders().begin()->second.executed == 1, "restart lost partial fill");
    require(restart.place(request("after_restart")).empty(), "restart remained recovery-only");
    require(restart.cancel(restart.orders().begin()->first).empty(), "recovered order cannot be cancelled");
    f.poll(restart, 3000);
    require(restart.orders().at("after_restart").ext_id > 102, "restart reused recovered ext_id");
    const auto executed = manager.orders().at("c").executed;
    ps::OwnTradeSnapshot alien;
    alien.id_deal = 99;
    alien.sess_id = 101;
    alien.isin_id = 42;
    alien.code_buy = "ABCD001";
    alien.ext_id_buy = manager.orders().at("c").ext_id;
    alien.public_order_id_buy = 999999;
    alien.amount = 100;
    manager.observe_trades(std::span(&alien, 1));
    require(manager.orders().at("c").executed == executed, "trade ext_id collision inflated fills");
}
void risk() {
    Fixture f;
    f.config.risk.max_quantity = 4;
    f.config.risk.max_open_orders = 2;
    auto manager = f.manager();
    require(!manager.place(request("oversize", 5)).empty(), "quantity limit ignored");
    auto off_tick = request("tick");
    off_tick.price = "100.5";
    require(!manager.place(off_tick).empty(), "tick alignment ignored");
    require(manager.place(request("one")).empty() && manager.place(request("two")).empty(), "reasonable order refused");
    require(!manager.place(request("three")).empty(), "open-order limit ignored");
    manager.set_kill_switch(true);
    require(!manager.place(request("kill")).empty(), "kill switch ignored");
    require(manager.cancel("one").empty(), "kill switch blocked cancel");
}
void cancel_races_and_identity() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("race")).empty(), "race Add refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    require(manager.move("race", "101", 2).empty(), "queued Move refused");
    require(manager.cancel("race").empty(), "cancel queued Move refused");
    f.poll(manager, 1000);
    require(std::none_of(f.sent.begin(), f.sent.end(),
                         [](auto item) { return item.kind == tr::Plaza2TradeCommandKind::MoveOrder; }),
            "unsent Move was dispatched after cancel request");
    const auto cancel_id = f.sent.back().id;
    manager.on_reply(cancel_id, {.msgid = 177, .amount = 3},
                     OrderManager::Clock::time_point{} + std::chrono::seconds(1));
    f.poll(manager, 62000);
    f.poll(manager, 64000);
    require(f.sent.back().id != cancel_id && f.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder,
            "accepted cancel lost its replication confirmation deadline");
    // Renumbering need not increase the exchange order id.
    Fixture renamed;
    auto other = renamed.manager();
    require(other.place(request("renamed")).empty(), "rename Add refused");
    renamed.poll(other, 0);
    other.on_reply(renamed.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    require(other.move("renamed", "101", 2).empty(), "rename Move refused");
    renamed.poll(other, 1000);
    other.on_reply(renamed.sent.back().id, {.msgid = 176, .order_id1 = 501},
                   OrderManager::Clock::time_point{} + std::chrono::seconds(1));
    observe(other, row(other.orders().at("renamed"), 501, 2, 1));
    observe(other, row(other.orders().at("renamed"), 1001, 0, 0));
    require(other.orders().at("renamed").order_id == 501 && !terminal(other.orders().at("renamed").state),
            "old-id removal cancelled a renamed working order");
}
void cancel_all_and_move_failures() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("all")).empty(), "bulk-cancel Add refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    require(manager.move("all", "101", 2).empty(), "bulk-cancel Move refused");
    require(manager.cancel_all(42).empty(), "bulk cancel refused");
    f.poll(manager, 1000);
    require(std::none_of(f.sent.begin(), f.sent.end(),
                         [](auto item) { return item.kind == tr::Plaza2TradeCommandKind::MoveOrder; }),
            "cancel-all dispatched an unsent Move");

    Fixture invalid;
    auto other = invalid.manager();
    require(other.place(request("invalid")).empty(), "validation Add refused");
    invalid.poll(other, 0);
    other.on_reply(invalid.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    observe(other, row(other.orders().at("invalid"), 1001, 2, 2));
    require(other.move("invalid", "101", 2).empty(), "validation Move refused");
    invalid.certainty = cg::Plaza2SubmissionCertainty::DefinitelyNotSent;
    invalid.validation_error = {.code = cg::Plaza2ErrorCode::InvalidConfiguration, .message = "layout changed"};
    invalid.poll(other, 1000);
    require(other.orders().at("invalid").state == OrderState::PartFilled &&
                other.orders().at("invalid").order_id == 1001,
            "unsent Move rejection lost the working exchange order");
    invalid.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
    invalid.validation_error = {};
    require(other.cancel("invalid").empty(), "ambiguous cancel refused");
    invalid.poll(other, 2000);
    require(other.orders().at("invalid").cancel_requested, "uncertain cancel did not retain risk-reduction intent");
}
void cancel_during_move() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("moving")).empty(), "moving Add refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    require(manager.move("moving", "101", 2).empty(), "in-flight Move refused");
    f.poll(manager, 1000);
    const auto move_id = f.sent.back().id;
    require(manager.cancel("moving").empty(), "cancel during Move refused");
    f.poll(manager, 1001);
    manager.on_reply(move_id, {.msgid = 176, .order_id1 = 501},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(1002));
    f.poll(manager, 1003);
    require(std::count_if(f.sent.begin(), f.sent.end(),
                          [](auto item) { return item.kind == tr::Plaza2TradeCommandKind::DelOrder; }) == 2,
            "pending old-id cancel suppressed cancellation of the Move result");
    require(manager.orders().at("moving").order_id == 501 && manager.orders().at("moving").cancel_requested,
            "Move reply lost cancel intent");
}
void replication_during_move() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("replace")).empty(), "replace Add refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    const auto cached = row(manager.orders().at("replace"), 1001, 3, 1);
    require(manager.move("replace", "101", 2).empty(), "first replacement refused");
    observe(manager, cached);
    require(manager.orders().at("replace").state == OrderState::PendingReplace &&
                !manager.move("replace", "102", 1).empty(),
            "cached replication released a queued Move");
    f.poll(manager, 1000);
    const auto move_id = f.sent.back().id;
    observe(manager, cached);
    require(manager.orders().at("replace").state == OrderState::PendingReplace &&
                !manager.move("replace", "102", 1).empty(),
            "cached replication released an in-flight Move");
    observe(manager, row(manager.orders().at("replace"), 1001, 0, 0));
    require(manager.orders().at("replace").state == OrderState::PendingReplace,
            "old-ID removal completed the replacement before its result");
    // TRADE can reveal the new ID before the asynchronous Move reply. It is
    // working, but another Move must wait until the outstanding reply resolves.
    observe(manager, row(manager.orders().at("replace"), 501, 2, 1));
    require(!manager.move("replace", "102", 1).empty(), "new-ID row released an unresolved Move reply");
    manager.on_reply(move_id, {.msgid = 176, .order_id1 = 501},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(1001));
    require(manager.orders().at("replace").order_id == 501 && manager.move("replace", "102", 1).empty(),
            "confirmed replacement remained blocked");
}
void confirmed_add_without_reply() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("confirmed")).empty(), "confirmed Add refused");
    f.poll(manager, 0);
    observe(manager, row(manager.orders().at("confirmed"), 1001, 3, 1));
    f.poll(manager, 62000);
    require(manager.orders().at("confirmed").state == OrderState::Working && f.sent.size() == 1 &&
                manager.queued() == 0,
            "lost Add reply cancelled a TRADE-confirmed working order");
    require(manager.move("confirmed", "101", 2).empty(), "TRADE-confirmed Add remained unresolved");

    Fixture uncertain;
    uncertain.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
    auto other = uncertain.manager();
    require(other.place(request("uncertain-confirmed")).empty(), "uncertain confirmed Add refused");
    uncertain.poll(other, 0);
    observe(other, row(other.orders().at("uncertain-confirmed"), 1002, 3, 1));
    uncertain.certainty = cg::Plaza2SubmissionCertainty::Posted;
    uncertain.poll(other, 1000);
    require(other.orders().at("uncertain-confirmed").state == OrderState::PendingCancel &&
                other.orders().at("uncertain-confirmed").cancel_requested &&
                std::any_of(uncertain.sent.begin(), uncertain.sent.end(),
                            [](auto command) { return command.kind == tr::Plaza2TradeCommandKind::DelOrder; }),
            "TRADE confirmation dropped an ambiguous Add's recovery cancel intent");
}
void cancelled_order_during_move() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("cancelled-before-move")).empty(), "queued cancelled Add refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    require(manager.move("cancelled-before-move", "101", 2).empty(), "queued cancelled Move refused");
    observe(manager, row(manager.orders().at("cancelled-before-move"), 1001, 0, 0));
    f.poll(manager, 1000);
    require(manager.orders().at("cancelled-before-move").state == OrderState::Cancelled && f.sent.size() == 1,
            "queued Move survived authoritative cancellation");

    Fixture posted;
    auto other = posted.manager();
    require(other.place(request("cancelled-with-move")).empty(), "posted cancelled Add refused");
    posted.poll(other, 0);
    other.on_reply(posted.sent.front().id, {.msgid = 179, .order_id = 1002}, OrderManager::Clock::time_point{});
    require(other.move("cancelled-with-move", "101", 2).empty(), "posted cancelled Move refused");
    posted.poll(other, 1000);
    observe(other, row(other.orders().at("cancelled-with-move"), 1002, 0, 0));
    other.on_reply(posted.sent.back().id, {.msgid = 176, .code = 17, .message = "order already removed"},
                   OrderManager::Clock::time_point{} + std::chrono::milliseconds(1001));
    require(other.orders().at("cancelled-with-move").state == OrderState::Cancelled,
            "rejected Move resurrected an authoritatively cancelled order");
}
} // namespace
int main() {
    try {
        burst();
        cancels();
        ambiguity();
        day_and_restart();
        risk();
        cancel_races_and_identity();
        cancel_all_and_move_failures();
        cancel_during_move();
        replication_during_move();
        confirmed_add_without_reply();
        cancelled_order_during_move();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
