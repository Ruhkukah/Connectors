#include "moex/connector_host/order_manager.hpp"

#include "fixtures/cgate99_messages.hpp"

#include <array>
#include <cstring>
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
    std::vector<std::byte> payload;
};
struct Fixture {
    bool ready{true};
    std::set<std::int32_t> unavailable_isins;
    bool throw_result_log{};
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
                sent.push_back({command.command_kind, id, ms, command.payload});
                return cg::Plaza2PublisherMessageResult{
                    .certainty = certainty,
                    .validation_error = validation_error,
                    .post_invoked = certainty != cg::Plaza2SubmissionCertainty::DefinitelyNotSent};
            },
            [this](auto isin) { return ready && !unavailable_isins.contains(isin); },
            [this](auto isin) { return terms(isin); },
            [this](auto kind, auto fields) {
                if (throw_result_log && kind == "command_result")
                    throw std::runtime_error("journal failure");
                log.push_back(std::string(kind) + std::string(fields));
            });
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
void fill(OrderManager& manager, std::int64_t order_id, std::int64_t deal_id, std::int64_t amount,
          std::int32_t session = 100) {
    ps::OwnTradeSnapshot trade;
    trade.id_deal = deal_id;
    trade.sess_id = session;
    trade.isin_id = 42;
    trade.code_buy = "ABCD001";
    trade.public_order_id_buy = order_id;
    trade.private_order_id_buy = order_id;
    trade.amount = amount;
    trade.price = "100";
    manager.observe_trades(std::span(&trade, 1));
}
template <class Wire> Wire wire(const Sent& sent) {
    require(sent.payload.size() == sizeof(Wire), "wire size mismatch");
    Wire decoded{};
    std::memcpy(&decoded, sent.payload.data(), sizeof(Wire));
    return decoded;
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
    const auto cancel_count = [&] {
        return std::count_if(f.sent.begin(), f.sent.end(),
                             [](const auto& sent) { return sent.kind == tr::Plaza2TradeCommandKind::DelOrder; });
    };
    const auto before_retry = cancel_count();
    f.poll(manager, 3000);
    require(cancel_count() == before_retry, "cancel ignored its first business-failure retry delay");
    f.poll(manager, 3001);
    require(cancel_count() == before_retry + 1, "failed cancel did not retry");
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
    fill(manager, 1001, 1, 1);
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
    auto relisted = row(manager.orders().at("c"), 2003, 3, 1, 101);
    relisted.id_ord1 = 1003;
    observe(manager, relisted);
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
    require(restart.orders().begin()->second.remaining == 1 && restart.orders().begin()->second.executed == 0 &&
                !restart.orders().begin()->second.execution_baseline_known,
            "restart invented an unavailable historical fill baseline");
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
void mass_cancel_priority_and_reconciliation() {
    Fixture f;
    f.config.max_commands_per_second = 30;
    f.config.risk.max_open_orders = 200;
    auto manager = f.manager();
    std::vector<ps::OwnOrderSnapshot> initial;
    for (std::int32_t index = 1; index <= 100; ++index) {
        ManagedOrder seed{.request = request("seed" + std::to_string(index)), .ext_id = index};
        auto value = row(seed, 1000 + index, 3, 1);
        value.trade_repl_commit_sequence = 1;
        initial.push_back(value);
    }
    manager.observe_orders(initial, true);
    require(manager.cancel("recovered:100:1001").empty(), "preexisting cancellation refused");
    require(manager.place(request("unsent-after-mass")).empty(), "unsent mass fixture Add refused");
    require(manager.cancel_all(42).empty(), "priority mass cancel refused");
    f.poll(manager, 0);
    require(f.sent.size() == 1 && f.sent.front().kind == tr::Plaza2TradeCommandKind::DelUserOrders,
            "mass cancel was delayed behind individual cancellations");
    const auto mass = f.sent.front();
    const auto payload = wire<official_cgate99::DelUserOrders>(mass);
    require(payload.buy_sell == 3 && payload.ext_id == 0 && payload.isin_id == 42,
            "priority mass cancel filters changed");
    require(manager.orders().at("unsent-after-mass").state == OrderState::Cancelled,
            "mass cancel left an unsent Add queued");
    f.poll(manager, 1000);
    require(f.sent.size() == 1, "individual cancellation ran before reply186");
    // A commit before186 is not the post-reply reconciliation point.
    auto survivor = initial.front();
    survivor.trade_repl_commit_sequence = 2;
    observe(manager, survivor);
    manager.on_reply(mass.id, {.msgid = 186, .num_orders = 99},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(1000));
    f.poll(manager, 1000);
    require(f.sent.size() == 1, "individual cancellation ran before a newer TRADE commit");
    std::vector<ps::OwnOrderSnapshot> terminal(initial.begin() + 1, initial.end());
    for (auto& value : terminal) {
        value.public_amount_rest = 0;
        value.public_action = 0;
        value.trade_repl_commit_sequence = 3;
    }
    survivor.trade_repl_commit_sequence = 3;
    terminal.push_back(survivor);
    manager.observe_orders(terminal);
    f.poll(manager, 1001);
    require(f.sent.size() == 2 && f.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder &&
                wire<official_cgate99::DelOrder>(f.sent.back()).order_id == 1001,
            "post186 TRADE reconciliation did not target only the surviving order");

    Fixture empty_commit;
    auto empty = empty_commit.manager();
    auto working = initial.front();
    working.trade_repl_commit_sequence = 5;
    observe(empty, working, true);
    require(empty.cancel_all(42).empty(), "empty-commit mass cancel refused");
    empty_commit.poll(empty, 0);
    empty.on_reply(empty_commit.sent.front().id, {.msgid = 186, .num_orders = 1}, OrderManager::Clock::time_point{}, 7);
    empty.observe_trade_commit(7);
    empty_commit.poll(empty, 1000);
    require(empty_commit.sent.size() == 1, "reply186 reused a commit already present in the same owner poll");
    empty.observe_trade_commit(8);
    empty_commit.poll(empty, 1001);
    require(empty_commit.sent.size() == 2 && empty_commit.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder,
            "empty TRADE commit did not release a surviving per-ID fallback");
}

void mass_cancel_supersedes_delayed_flood_replies() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("sent-cancel")).empty() && manager.place(request("sent-move")).empty(),
            "delayed flood fixture Adds refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.at(0).id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    manager.on_reply(f.sent.at(1).id, {.msgid = 179, .order_id = 1002}, OrderManager::Clock::time_point{});
    require(manager.cancel("sent-cancel").empty(), "delayed flood cancellation refused");
    require(manager.move("sent-move", "101", 2).empty(), "delayed flood Move refused");
    f.poll(manager, 0);
    const auto individual = f.sent.at(2), move = f.sent.at(3);
    require(individual.kind == tr::Plaza2TradeCommandKind::DelOrder &&
                move.kind == tr::Plaza2TradeCommandKind::MoveOrder,
            "delayed flood commands not in flight");
    require(manager.cancel_all(42).empty(), "delayed flood mass cancel refused");
    f.poll(manager, 0);
    const auto mass = f.sent.at(4);
    require(mass.kind == tr::Plaza2TradeCommandKind::DelUserOrders, "delayed flood mass request not sent first");
    manager.on_reply(individual.id, {.msgid = 99, .penalty_remain = 2000}, OrderManager::Clock::time_point{});
    manager.on_reply(move.id, {.msgid = 99, .penalty_remain = 3000}, OrderManager::Clock::time_point{});
    require(manager.queued() == 0, "superseded in-flight99 requeued an individual cancel or Move");
    f.poll(manager, 1000);
    require(f.sent.size() == 5, "superseded commands ran before mass reconciliation");
    manager.on_reply(mass.id, {.msgid = 186, .num_orders = 1},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(1000), 7);
    auto terminal_order = row(manager.orders().at("sent-cancel"), 1001, 0, 0);
    auto survivor = row(manager.orders().at("sent-move"), 1002, 3, 1);
    terminal_order.trade_repl_commit_sequence = survivor.trade_repl_commit_sequence = 8;
    const std::array reconciled{terminal_order, survivor};
    manager.observe_orders(reconciled);
    f.poll(manager, 2999);
    require(f.sent.size() == 5, "discarded command99 did not apply the global flood penalty");
    f.poll(manager, 3000);
    require(f.sent.size() == 6 && f.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder &&
                wire<official_cgate99::DelOrder>(f.sent.back()).order_id == 1002,
            "mass fallback resurrected a discarded Move or retried a terminal order");
    require(std::count_if(f.sent.begin(), f.sent.end(),
                          [](const auto& sent) { return sent.kind == tr::Plaza2TradeCommandKind::MoveOrder; }) == 1,
            "cancel_requested Move was resubmitted after a delayed99");

    Fixture explicit_cancel;
    auto single = explicit_cancel.manager();
    require(single.place(request("explicit-cancel")).empty(), "explicit cancellation Add refused");
    explicit_cancel.poll(single, 0);
    single.on_reply(explicit_cancel.sent.front().id, {.msgid = 179, .order_id = 1001},
                    OrderManager::Clock::time_point{});
    require(single.move("explicit-cancel", "101", 2).empty(), "explicit cancellation Move refused");
    explicit_cancel.poll(single, 0);
    const auto rejected_move = explicit_cancel.sent.back();
    require(single.cancel("explicit-cancel").empty(), "explicit cancellation refused");
    explicit_cancel.poll(single, 0);
    single.on_reply(rejected_move.id, {.msgid = 99, .penalty_remain = 2000}, OrderManager::Clock::time_point{});
    explicit_cancel.poll(single, 2000);
    require(explicit_cancel.sent.size() == 3 && single.queued() == 0 &&
                single.orders().at("explicit-cancel").cancel_requested,
            "delayed99 revived a Move superseded by an explicit individual cancellation");

    Fixture replacement;
    auto replaced = replacement.manager();
    ManagedOrder seed{.request = request("replacement-seed"), .ext_id = 1};
    auto live = row(seed, 2001, 3, 1);
    live.trade_repl_commit_sequence = 1;
    observe(replaced, live, true);
    require(replaced.cancel_all(42).empty(), "first mass request refused");
    replacement.poll(replaced, 0);
    const auto old_mass = replacement.sent.back();
    require(replaced.cancel_all(42).empty(), "replacement mass request refused");
    replacement.poll(replaced, 0);
    const auto new_mass = replacement.sent.back();
    replaced.on_reply(old_mass.id, {.msgid = 99, .penalty_remain = 2000}, OrderManager::Clock::time_point{});
    replaced.on_reply(new_mass.id, {.msgid = 186, .num_orders = 1}, OrderManager::Clock::time_point{}, 1);
    replaced.observe_trade_commit(2);
    replacement.poll(replaced, 1999);
    require(replacement.sent.size() == 2, "superseded mass99 lost its global flood penalty");
    replacement.poll(replaced, 2000);
    require(replacement.sent.size() == 3 && replacement.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder,
            "superseded mass99 was requeued or lost the current group's fallback");
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
    fill(other, 1001, 1, 1);
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
    require(manager.orders().at("replace").executed == 0, "early replacement row invented executions");
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
void flood_does_not_exhaust_cancel_budget() {
    for (const bool recovery : {false, true}) {
        Fixture f;
        auto manager = f.manager();
        if (recovery)
            f.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
        require(manager.place(request("flood-cancel")).empty(), "flood fixture Add refused");
        f.poll(manager, 0);
        if (!recovery) {
            manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
            require(manager.cancel("flood-cancel").empty(), "flood fixture cancel refused");
        }
        f.certainty = cg::Plaza2SubmissionCertainty::Posted;
        f.poll(manager, 0);
        const auto expected =
            recovery ? tr::Plaza2TradeCommandKind::DelUserOrders : tr::Plaza2TradeCommandKind::DelOrder;
        for (std::int64_t attempt = 0; attempt < 5; ++attempt) {
            require(f.sent.back().kind == expected, "flood retry changed cancellation kind");
            const auto sent = f.sent.back();
            const auto now = attempt * 2000;
            manager.on_reply(sent.id, {.msgid = 99, .penalty_remain = 2000},
                             OrderManager::Clock::time_point{} + std::chrono::milliseconds(now));
            require(!manager.operator_action_required(), "99 exhausted the business cancel retry budget");
            const auto count = f.sent.size();
            f.poll(manager, now + 1999);
            require(f.sent.size() == count, "flood cancellation ignored penalty_remain");
            f.poll(manager, now + 2000);
            require(f.sent.size() == count + 1 && f.sent.back().id != sent.id,
                    "flood cancellation did not retry with a fresh correlation ID");
        }
        // Floods must not consume any of the three genuine failure attempts.
        for (std::uint32_t rejected = 0; rejected < 3; ++rejected) {
            const auto now = f.ms;
            const auto before = f.sent.size();
            manager.on_reply(f.sent.back().id, {.msgid = recovery ? 186 : 177, .code = 17},
                             OrderManager::Clock::time_point{} + std::chrono::milliseconds(now));
            require(manager.operator_action_required() == (rejected == 2),
                    "floods consumed part of the business failure budget");
            f.poll(manager, now + (rejected == 0 ? 1000 : 2000));
            require(f.sent.size() == before + (rejected == 2 ? 0 : 1), "business retry bound changed after flood");
        }
    }

    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("moving-flood")).empty() && manager.place(request("priority-cancel")).empty(),
            "Move flood fixture Adds refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.at(0).id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    manager.on_reply(f.sent.at(1).id, {.msgid = 179, .order_id = 1101}, OrderManager::Clock::time_point{});
    require(manager.move("moving-flood", "101", 2).empty(), "Move flood request refused");
    f.poll(manager, 1000);
    const auto move = f.sent.back();
    manager.on_reply(move.id, {.msgid = 99, .penalty_remain = 2000},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(1000));
    require(manager.orders().at("moving-flood").state == OrderState::PendingReplace &&
                !manager.operator_action_required(),
            "Move flood changed pending state");
    require(manager.cancel("priority-cancel").empty(), "priority cancellation refused");
    // A real cancellation must overtake the delayed Move when the flood penalty ends.
    f.poll(manager, 2999);
    require(f.sent.size() == 3, "Move flood penalty ignored");
    f.poll(manager, 3000);
    require(f.sent.at(3).kind == tr::Plaza2TradeCommandKind::DelOrder &&
                f.sent.at(4).kind == tr::Plaza2TradeCommandKind::MoveOrder && f.sent.at(4).id != move.id,
            "Move flood entered the cancellation queue or lost its replacement");
    require(wire<official_cgate99::MoveOrder>(f.sent.at(4)).regime == 3 && f.sent.at(4).payload == move.payload,
            "Move flood retry altered exchange payload");
    manager.on_reply(f.sent.at(4).id, {.msgid = 176, .order_id1 = 1002},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(3000));
    require(manager.orders().at("moving-flood").order_id == 1002 && manager.orders().at("moving-flood").remaining == 2,
            "Move flood retry did not correlate reply176");
}

void recovery_bounds_and_wire() {
    Fixture f;
    f.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
    auto manager = f.manager();
    require(manager.place(request("bounded")).empty(), "bounded Add refused");
    f.poll(manager, 0);
    const auto original_send = manager.orders().at("bounded").sent_utc_seconds;
    f.certainty = cg::Plaza2SubmissionCertainty::Posted;
    std::size_t handled{};
    for (std::int64_t ms = 0; ms <= 130000; ms += 100) {
        f.poll(manager, ms);
        while (handled < f.sent.size()) {
            const auto sent = f.sent[handled++];
            if (sent.kind != tr::Plaza2TradeCommandKind::DelUserOrders)
                continue;
            const auto payload = wire<official_cgate99::DelUserOrders>(sent);
            require(payload.buy_sell == 3 && payload.non_system == 0 && payload.instrument_mask == 1,
                    "recovery cancel has invalid exchange filter bytes");
            manager.on_reply(sent.id, {.msgid = 186, .code = 17, .message = "recovery rejected"},
                             OrderManager::Clock::time_point{} + std::chrono::milliseconds(ms));
            require(manager.orders().at("bounded").state == OrderState::Unknown,
                    "failed recovery changed Unknown into Working");
        }
    }
    std::vector<std::int64_t> sent_at;
    for (const auto& sent : f.sent)
        if (sent.kind == tr::Plaza2TradeCommandKind::DelUserOrders)
            sent_at.push_back(sent.ms);
    require(sent_at == std::vector<std::int64_t>({0, 1000, 3000}),
            "recovery attempts lack bounded exponential backoff");
    require(manager.orders().at("bounded").operator_action_required && manager.operator_action_required(),
            "exhausted recovery lacks explicit operator status");
    require(manager.orders().at("bounded").sent_utc_seconds == original_send,
            "retry changed first-send absence anchor");
    require(manager.cancel("bounded").empty(), "operator cannot explicitly retry exhausted recovery");
    f.poll(manager, 131000);
    require(f.sent.back().kind == tr::Plaza2TradeCommandKind::DelUserOrders &&
                !manager.orders().at("bounded").operator_action_required,
            "explicit operator retry remained disabled");

    Fixture bulk;
    auto other = bulk.manager();
    require(other.cancel_all(42).empty(), "bulk cancellation refused");
    std::size_t count{};
    for (std::int64_t ms = 0; ms <= 10000; ms += 100) {
        bulk.poll(other, ms);
        while (count < bulk.sent.size()) {
            const auto sent = bulk.sent[count++];
            require(wire<official_cgate99::DelUserOrders>(sent).buy_sell == 3, "cancel-all direction bytes invalid");
            other.on_reply(sent.id, {.msgid = 100}, OrderManager::Clock::time_point{} + std::chrono::milliseconds(ms));
        }
    }
    require(bulk.sent.size() == 3 && other.operator_action_required(), "failed cancel-all retries forever");
}
void ext_identity_and_relist() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("owned")).empty(), "identity Add refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 2001}, OrderManager::Clock::time_point{});
    auto foreign = row(manager.orders().at("owned"), 7777, 3, 1);
    foreign.client_code = "ABCD999";
    observe(manager, foreign);
    foreign.client_code = "ABCD001";
    observe(manager, foreign);
    require(manager.orders().at("owned").order_id == 2001, "ext_id collision renamed a known order");
    require(manager.cancel("owned").empty(), "owned cancellation refused");
    f.poll(manager, 1000);
    require(wire<official_cgate99::DelOrder>(f.sent.back()).order_id == 2001,
            "ext_id collision redirected a cancellation");

    Fixture pending;
    auto unknown = pending.manager();
    require(unknown.place(request("pending")).empty(), "unresolved identity Add refused");
    pending.poll(unknown, 0);
    const auto own = unknown.orders().at("pending");
    for (int variant = 0; variant < 3; ++variant) {
        auto mismatch = row(own, 8000 + variant, 3, 1);
        if (variant == 0)
            mismatch.isin_id = 43;
        if (variant == 1)
            mismatch.sess_id = 99;
        if (variant == 2)
            mismatch.dir = 2;
        observe(unknown, mismatch);
    }
    require(unknown.orders().at("pending").order_id == 0, "ext_id fallback ignored the order contract");
    observe(unknown, row(own, 9001, 3, 1));
    require(unknown.orders().at("pending").order_id == 9001, "valid own TRADE Add confirmation rejected");
    auto old_delete = row(unknown.orders().at("pending"), 9001, 0, 0);
    auto new_day = row(unknown.orders().at("pending"), 9101, 3, 1, 101);
    new_day.ext_id = 0;
    new_day.id_ord1 = 9001;
    pending.session = 101;
    const auto before = pending.log.size();
    std::array batch{old_delete, new_day};
    unknown.observe_orders(batch);
    require(unknown.orders().at("pending").order_id == 9101 && unknown.orders().size() == 4,
            "documented day linkage lost the logical order");
    require(std::none_of(pending.log.begin() + static_cast<std::ptrdiff_t>(before), pending.log.end(),
                         [](const auto& line) { return line.find("\"state\":\"Cancelled\"") != std::string::npos; }),
            "transactional relist published an intermediate terminal state");
}
void move_fill_accounting() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("fills")).empty(), "fill race Add refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    fill(manager, 1001, 1, 1);
    observe(manager, row(manager.orders().at("fills"), 1001, 2, 2));
    require(manager.move("fills", "101", 3).empty(), "fill race Move refused");
    f.poll(manager, 1000);
    const auto first_move = f.sent.back();
    const auto first_wire = wire<official_cgate99::MoveOrder>(first_move);
    require(first_wire.regime == 3 && first_wire.amount1 == 3, "Move wire can replenish racing fills");
    fill(manager, 1001, 2, 1);
    auto replacement = row(manager.orders().at("fills"), 501, 1, 1);
    observe(manager, replacement);
    require(manager.orders().at("fills").order_id == 1001 && manager.orders().at("fills").executed == 2,
            "early replacement row changed ownership or fabricated executions");
    manager.on_reply(first_move.id, {.msgid = 176, .order_id1 = 501},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(1001));
    require(manager.orders().at("fills").remaining == 1 && manager.orders().at("fills").executed == 2,
            "Move reply reset a racing fill or authoritative remaining quantity");
    require(manager.move("fills", "102", 4).empty(), "second logical-total Move refused");
    f.poll(manager, 2000);
    const auto second = wire<official_cgate99::MoveOrder>(f.sent.back());
    require(second.regime == 3 && second.amount1 == 2, "second Move counted fills from previous order IDs twice");
    const auto executed = manager.orders().at("fills").executed;
    fill(manager, 1001, 2, 1);
    require(manager.orders().at("fills").executed == executed, "replayed user_deal counted twice");
}

void deferred_fill_identity() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("buy")).empty(), "deferred buy Add refused");
    auto sell_request = request("sell");
    sell_request.side = tr::Plaza2TradeSide::Sell;
    require(manager.place(sell_request).empty(), "deferred sell Add refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    ps::OwnTradeSnapshot self;
    self.sess_id = 100;
    self.isin_id = 42;
    self.id_deal = 41;
    self.code_buy = self.code_sell = "ABCD001";
    self.public_order_id_buy = self.private_order_id_buy = 1001;
    self.public_order_id_sell = self.private_order_id_sell = 1002;
    self.amount = 1;
    self.price = "100";
    manager.observe_trades(std::span(&self, 1));
    require(manager.orders().at("buy").executed == 1 && manager.orders().at("sell").executed == 0,
            "unmapped self-trade side was attributed by ext_id");
    manager.on_reply(f.sent.at(1).id, {.msgid = 179, .order_id = 1002}, OrderManager::Clock::time_point{});
    manager.observe_trades(std::span(&self, 1));
    require(manager.orders().at("buy").executed == 1 && manager.orders().at("sell").executed == 1,
            "deferred self-trade side lost or double-counted after reply mapping");

    require(manager.move("buy", "101", 5).empty(), "whole-fill Move refused");
    f.poll(manager, 1000);
    const auto move_id = f.sent.back().id;
    fill(manager, 1001, 42, 2);
    observe(manager, row(manager.orders().at("buy"), 1001, 0, 2));
    require(manager.orders().at("buy").state == OrderState::PendingReplace,
            "old-ID final fill discarded an outstanding Move");
    fill(manager, 1003, 43, 1);
    manager.on_reply(move_id, {.msgid = 176, .order_id1 = 1003}, OrderManager::Clock::time_point{});
    require(manager.orders().at("buy").order_id == 1003 && manager.orders().at("buy").executed == 4 &&
                manager.orders().at("buy").remaining == 1,
            "replacement-only reply lost early fill or logical target quantity");
    require(manager.cancel_all(42).empty(), "replacement mass cancel refused");
    f.poll(manager, 2000);
    std::array cancellations{row(manager.orders().at("buy"), 1003, 0, 0), row(manager.orders().at("sell"), 1002, 0, 0)};
    cancellations[1].dir = 2;
    manager.observe_orders(cancellations);
    require(manager.orders().at("buy").state == OrderState::Cancelled &&
                manager.orders().at("sell").state == OrderState::Cancelled,
            "mass cancel did not settle replacement terminal replication");
}

void eligible_commands_and_logging() {
    Fixture f;
    auto manager = f.manager();
    for (const auto key : {"delayed", "eligible"}) {
        require(manager.place(request(key)).empty(), "eligible Add refused");
        f.poll(manager, 0);
        manager.on_reply(f.sent.back().id, {.msgid = 179, .order_id = key == std::string("delayed") ? 1001 : 1002},
                         OrderManager::Clock::time_point{});
    }
    require(manager.cancel("delayed").empty(), "delayed cancel refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.back().id, {.msgid = 177, .code = 17}, OrderManager::Clock::time_point{});
    require(manager.cancel("eligible").empty(), "eligible cancel refused");
    require(manager.place(request("new")).empty(), "new eligible Add refused");
    f.poll(manager, 1);
    require(wire<official_cgate99::DelOrder>(f.sent.at(3)).order_id == 1002 &&
                f.sent.at(4).kind == tr::Plaza2TradeCommandKind::AddOrder,
            "backoff at queue head blocked an eligible cancel/Add");

    Fixture instruments;
    auto independent = instruments.manager();
    require(independent.place(request("paused")).empty(), "pausing Add refused");
    auto other_request = request("other-isin");
    other_request.isin_id = 43;
    require(independent.place(other_request).empty(), "other ISIN Add refused");
    instruments.unavailable_isins.insert(42);
    instruments.poll(independent, 0);
    require(instruments.sent.size() == 1 && wire<official_cgate99::AddOrder>(instruments.sent.front()).isin_id == 43,
            "paused instrument blocked an independent ready Add");
    instruments.session = 101;
    instruments.unavailable_isins.clear();
    instruments.poll(independent, 1000);
    observe(independent, row(independent.orders().at("paused"), 3001, 3, 1, 101));
    require(independent.orders().at("paused").order_id == 3001 && independent.orders().at("paused").sess_id == 101,
            "queued Add retained a stale session for ext_id reconciliation");

    Fixture throttle;
    throttle.config.max_commands_per_second = 1;
    auto limited = throttle.manager();
    require(limited.place(request("one")).empty() && limited.place(request("two")).empty(), "throttle Adds refused");
    for (int ms = 0; ms < 1000; ++ms)
        throttle.poll(limited, ms);
    require(std::count_if(throttle.log.begin(), throttle.log.end(),
                          [](const auto& line) {
                              return line.starts_with("throttle") && line.find("\"active\":true") != std::string::npos;
                          }) == 1,
            "throttle logs repeat on every blocked poll");
    throttle.poll(limited, 1000);
    require(std::count_if(throttle.log.begin(), throttle.log.end(),
                          [](const auto& line) {
                              return line.starts_with("throttle") && line.find("\"active\":false") != std::string::npos;
                          }) == 1,
            "throttle exit transition missing");

    Fixture logs;
    logs.throw_result_log = true;
    auto guarded = logs.manager();
    require(guarded.place(request("logged")).empty(), "logging Add refused");
    logs.poll(guarded, 0);
    guarded.on_reply(logs.sent.front().id, {.msgid = 179, .order_id = 1003}, OrderManager::Clock::time_point{});
    require(guarded.orders().at("logged").order_id == 1003, "post-send log exception lost pending correlation");
    require(!guarded.place(request("after-log-failure")).empty(), "failed interaction logging permits new risk");
}
void aggregate_risk() {
    Fixture f;
    f.config.risk.max_notional_scaled = 50'000'000;
    auto manager = f.manager();
    require(manager.place(request("first", 3)).empty(), "first under-cap Add refused");
    require(!manager.place(request("second", 3)).empty(), "individually valid orders exceeded aggregate cap");
    f.poll(manager, 0);
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    require(manager.move("first", "101", 3).empty(), "Move double-counted its current order in aggregate cap");
    require(manager.place(request("fits", 1)).empty(), "conservative replacement reservation rejected a fitting Add");
    auto outside = row(manager.orders().at("first"), 8001, 1, 1);
    outside.ext_id = 500;
    outside.isin_id = 43;
    observe(manager, outside);
    require(!manager.place(request("over", 1)).empty(), "recovered own order outside configured ISIN did not count");
    require(manager.cancel_all(42).empty(), "aggregate cap blocked emergency cancellation");
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
        mass_cancel_priority_and_reconciliation();
        mass_cancel_supersedes_delayed_flood_replies();
        cancel_all_and_move_failures();
        cancel_during_move();
        replication_during_move();
        confirmed_add_without_reply();
        cancelled_order_during_move();
        flood_does_not_exhaust_cancel_budget();
        recovery_bounds_and_wire();
        ext_identity_and_relist();
        move_fill_accounting();
        deferred_fill_identity();
        eligible_commands_and_logging();
        aggregate_risk();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
