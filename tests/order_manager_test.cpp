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
    for (std::size_t i = 0; i < rows.size(); ++i)
        manager.on_reply(f.sent.at(i).id, {.msgid = 179, .order_id = rows[i].public_order_id},
                         OrderManager::Clock::time_point{});
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

void rejected_mass_cancel_falls_back_to_each_known_id() {
    Fixture f;
    auto manager = f.manager();
    ManagedOrder buy{.request = request("bulk-buy"), .ext_id = 1};
    ManagedOrder sell{.request = request("bulk-sell"), .ext_id = 2};
    auto first = row(buy, 1101, 3, 1);
    auto second = row(sell, 1102, 3, 1);
    second.dir = 2;
    auto unaffected = row(buy, 1103, 3, 1);
    unaffected.isin_id = 43;
    std::array initial{first, second, unaffected};
    manager.observe_orders(initial, true);
    require(manager.cancel_all(42).empty(), "rejected mass cancel refused");
    f.poll(manager, 0);
    for (const auto now : {0, 1000, 3000}) {
        require(f.sent.back().kind == tr::Plaza2TradeCommandKind::DelUserOrders,
                "bulk retry changed command before exhausting its own budget");
        manager.on_reply(f.sent.back().id, {.msgid = 186, .code = 17},
                         OrderManager::Clock::time_point{} + std::chrono::milliseconds(now));
        if (now < 3000)
            f.poll(manager, now == 0 ? 1000 : 3000);
    }
    f.poll(manager, 3000);
    const auto del_count = [&] {
        return std::count_if(f.sent.begin(), f.sent.end(),
                             [](const auto& sent) { return sent.kind == tr::Plaza2TradeCommandKind::DelOrder; });
    };
    require(del_count() == 2, "three bulk business rejections did not post individual fallback for each working ID");
    std::set<std::int64_t> targets;
    for (const auto& sent : f.sent)
        if (sent.kind == tr::Plaza2TradeCommandKind::DelOrder)
            targets.insert(wire<official_cgate99::DelOrder>(sent).order_id);
    require(targets == std::set<std::int64_t>{1101, 1102} &&
                !manager.orders().at("recovered:100:1103").cancel_requested,
            "rejected bulk fallback changed ownership, side or instrument scope");
    const auto last_cancel = [&](std::int64_t target) {
        const auto sent = std::find_if(f.sent.rbegin(), f.sent.rend(), [&](const auto& value) {
            return value.kind == tr::Plaza2TradeCommandKind::DelOrder &&
                   wire<official_cgate99::DelOrder>(value).order_id == target;
        });
        require(sent != f.sent.rend(), "individual fallback missing");
        return sent->id;
    };
    for (int failure = 0; failure < 3; ++failure) {
        const auto now = failure == 0 ? 3000 : failure == 1 ? 4000 : 6000;
        const auto before = del_count();
        manager.on_reply(last_cancel(1101), {.msgid = 177, .code = 17},
                         OrderManager::Clock::time_point{} + std::chrono::milliseconds(now));
        require(manager.orders().at("recovered:100:1101").operator_action_required == (failure == 2) &&
                    !manager.orders().at("recovered:100:1102").operator_action_required,
                "bulk business budget leaked into individual fallback budget");
        f.poll(manager, now + (failure == 0 ? 1000 : 2000));
        require(del_count() == before + (failure == 2 ? 0 : 1),
                "individual fallback lost its own bounded business-rejection retries");
    }
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
void uncertain_cancels_preserve_identity_and_budget() {
    Fixture f;
    f.config.reply_timeout = std::chrono::milliseconds(100);
    auto manager = f.manager();
    require(manager.place(request("known-cancel")).empty(), "known cancellation Add refused");
    f.poll(manager, 0);
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    require(manager.cancel("known-cancel").empty(), "known cancellation refused");
    f.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
    f.poll(manager, 0);
    require(manager.orders().at("known-cancel").state != OrderState::Unknown &&
                manager.orders().at("known-cancel").order_id == 1001 && f.sent.size() == 2,
            "PossiblySent DelOrder erased known identity or created an ext_id mass cancel");
    f.certainty = cg::Plaza2SubmissionCertainty::Posted;
    for (std::int64_t cycle = 0; cycle < 5; ++cycle) {
        const auto posted_at = cycle * 1100;
        const auto id = f.sent.back().id;
        if (cycle % 2 == 0)
            manager.on_reply(id, {.msgid = 177},
                             OrderManager::Clock::time_point{} + std::chrono::milliseconds(posted_at));
        else
            manager.on_reply(id, {.msgid = 179, .order_id = 9999},
                             OrderManager::Clock::time_point{} + std::chrono::milliseconds(posted_at));
        const auto before = f.sent.size();
        f.poll(manager, posted_at + 100);
        require(!manager.operator_action_required() &&
                    manager.orders().at("known-cancel").state != OrderState::Unknown &&
                    manager.orders().at("known-cancel").order_id == 1001,
                "lost cancellation reply/confirmation exhausted the business budget or lost identity");
        f.poll(manager, posted_at + 1099);
        require(f.sent.size() == before, "uncertain cancellation retry ignored pacing");
        f.poll(manager, posted_at + 1100);
        require(f.sent.size() == before + 1 && f.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder &&
                    f.sent.back().id != id,
                "uncertain known-ID cancellation stopped retrying");
    }
    // The five unknown outcomes leave all genuine business-rejection attempts available.
    for (std::uint32_t failure = 0; failure < 3; ++failure) {
        const auto before = f.sent.size();
        const auto now = f.ms;
        manager.on_reply(f.sent.back().id, {.msgid = 177, .code = 17},
                         OrderManager::Clock::time_point{} + std::chrono::milliseconds(now));
        require(manager.operator_action_required() == (failure == 2),
                "unknown cancellation outcomes spent the business failure budget");
        f.poll(manager, now + (failure == 0 ? 1000 : 2000));
        require(f.sent.size() == before + (failure == 2 ? 0 : 1), "business rejection retry bound changed");
    }

    Fixture bulk;
    bulk.config.reply_timeout = std::chrono::milliseconds(100);
    auto group = bulk.manager();
    ManagedOrder seed{.request = request("bulk-timeout-seed"), .ext_id = 1};
    auto live = row(seed, 2001, 3, 1);
    live.trade_repl_commit_sequence = 1;
    observe(group, live, true);
    require(group.cancel_all(42).empty(), "uncertain bulk request refused");
    bulk.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
    bulk.poll(group, 0);
    bulk.certainty = cg::Plaza2SubmissionCertainty::Posted;
    for (std::int64_t cycle = 0; cycle < 5; ++cycle) {
        const auto posted_at = cycle * 1100;
        group.on_timeout(bulk.sent.back().id,
                         OrderManager::Clock::time_point{} + std::chrono::milliseconds(posted_at + 100));
        live.trade_repl_commit_sequence = static_cast<std::uint64_t>(cycle + 2);
        observe(group, live);
        bulk.poll(group, posted_at + 1100);
        require(
            !group.operator_action_required() && bulk.sent.size() == static_cast<std::size_t>(cycle + 2) &&
                std::all_of(bulk.sent.begin(), bulk.sent.end(),
                            [](const auto& sent) { return sent.kind == tr::Plaza2TradeCommandKind::DelUserOrders; }),
            "bulk timeout exhausted business retries or dropped the reconciliation gate");
    }
    // A system100 is still an unknown result, including a nonzero system code.
    const auto before = bulk.sent.size();
    const auto now = bulk.ms;
    group.on_reply(bulk.sent.back().id, {.msgid = 100, .code = 1},
                   OrderManager::Clock::time_point{} + std::chrono::milliseconds(now));
    bulk.poll(group, now + 1000);
    require(!group.operator_action_required() && bulk.sent.size() == before + 1 &&
                bulk.sent.back().kind == tr::Plaza2TradeCommandKind::DelUserOrders,
            "system100 spent the bulk business retry budget");
    for (int confirmation = 0; confirmation < 4; ++confirmation) {
        const auto accepted_at = bulk.ms;
        const auto sent_before_confirmation = bulk.sent.size();
        group.on_reply(bulk.sent.back().id, {.msgid = 186, .num_orders = 1},
                       OrderManager::Clock::time_point{} + std::chrono::milliseconds(accepted_at), 6);
        bulk.poll(group, accepted_at + 100);
        bulk.poll(group, accepted_at + 1099);
        require(bulk.sent.size() == sent_before_confirmation, "accepted bulk confirmation retry ignored pacing");
        bulk.poll(group, accepted_at + 1100);
        require(!group.operator_action_required() && bulk.sent.size() == sent_before_confirmation + 1 &&
                    bulk.sent.back().kind == tr::Plaza2TradeCommandKind::DelUserOrders,
                "accepted bulk without a TRADE commit stopped retrying or lost its group gate");
    }
    live.public_amount_rest = 0;
    live.public_action = 0;
    live.trade_repl_commit_sequence = 7;
    observe(group, live);
    const auto after = bulk.sent.size();
    bulk.poll(group, bulk.ms + 10000);
    require(group.orders().at("recovered:100:2001").state == OrderState::Cancelled && bulk.sent.size() == after,
            "authoritative bulk terminal row did not stop retries");
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
    require(manager.orders().at("confirmed").state == OrderState::Unknown &&
                manager.orders().at("confirmed").order_id == 0 &&
                std::count_if(f.sent.begin(), f.sent.end(),
                              [](const auto& sent) { return sent.kind == tr::Plaza2TradeCommandKind::AddOrder; }) ==
                    1 &&
                f.sent.back().kind == tr::Plaza2TradeCommandKind::DelUserOrders,
            "lost179 guessed identity or blindly resent the Add instead of ext recovery");
    require(!manager.move("confirmed", "101", 2).empty(), "ext-only identity permitted a Move");

    Fixture uncertain;
    uncertain.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
    auto other = uncertain.manager();
    require(other.place(request("uncertain-confirmed")).empty(), "uncertain confirmed Add refused");
    uncertain.poll(other, 0);
    observe(other, row(other.orders().at("uncertain-confirmed"), 1002, 3, 1));
    uncertain.certainty = cg::Plaza2SubmissionCertainty::Posted;
    uncertain.poll(other, 1000);
    require(other.orders().at("uncertain-confirmed").state == OrderState::Unknown &&
                other.orders().at("uncertain-confirmed").order_id == 0 &&
                other.orders().at("uncertain-confirmed").cancel_requested &&
                std::none_of(uncertain.sent.begin(), uncertain.sent.end(),
                             [](auto command) { return command.kind == tr::Plaza2TradeCommandKind::DelOrder; }),
            "ext-only row guessed a direct cancellation identity");
    other.on_reply(uncertain.sent.front().id, {.msgid = 179, .order_id = 1002},
                   OrderManager::Clock::time_point{} + std::chrono::milliseconds(1001));
    uncertain.poll(other, 1002);
    require(other.orders().at("uncertain-confirmed").order_id == 1002 &&
                other.orders().at("uncertain-confirmed").cancel_requested &&
                std::any_of(uncertain.sent.begin(), uncertain.sent.end(),
                            [](auto command) { return command.kind == tr::Plaza2TradeCommandKind::DelOrder; }),
            "official179 did not preserve the ambiguous Add's cancellation intent");
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
            other.on_reply(sent.id, {.msgid = 186, .code = 17},
                           OrderManager::Clock::time_point{} + std::chrono::milliseconds(ms));
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
    require(unknown.orders().at("pending").order_id == 0, "ext-only TRADE row established Add identity");
    auto old_delete = row(unknown.orders().at("pending"), 9001, 0, 0);
    auto new_day = row(unknown.orders().at("pending"), 9101, 3, 1, 101);
    new_day.ext_id = 0;
    new_day.id_ord1 = 9001;
    pending.session = 101;
    const auto before = pending.log.size();
    std::array batch{old_delete, new_day};
    unknown.observe_orders(batch);
    unknown.on_reply(pending.sent.front().id, {.msgid = 179, .order_id = 9001}, OrderManager::Clock::time_point{});
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
    require(independent.orders().at("paused").order_id == 0 && independent.orders().at("paused").sess_id == 101,
            "queued Add retained a stale submission session or accepted ext-only identity");
    independent.on_reply(instruments.sent.back().id, {.msgid = 179, .order_id = 3001},
                         OrderManager::Clock::time_point{} + std::chrono::milliseconds(1001));
    require(independent.orders().at("paused").order_id == 3001 && independent.orders().at("paused").sess_id == 101,
            "official179 lost the queued Add's current submission session");

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
void terminal_pruning_and_relist() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("carry")).empty(), "carried Add refused");
    f.poll(manager, 0);
    const auto old_reply = f.sent.front().id;
    manager.on_reply(old_reply, {.msgid = 179, .order_id = 9001}, OrderManager::Clock::time_point{});
    require(manager.cancel("carry").empty(), "carried cancellation refused");
    f.poll(manager, 1);
    const auto old_cancel = f.sent.back().id;
    require(manager.place(request("old-live")).empty(), "old-session live Add refused");
    f.poll(manager, 2);
    manager.on_reply(f.sent.back().id, {.msgid = 179, .order_id = 9301}, OrderManager::Clock::time_point{});
    require(manager.place(request("old-unknown")).empty(), "old-session uncertain Add refused");
    f.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
    f.poll(manager, 3);
    f.certainty = cg::Plaza2SubmissionCertainty::Posted;
    auto old = row(manager.orders().at("carry"), 9001, 0, 0);
    observe(manager, old);
    // A different own row advances the session between delete and relist commits.
    auto current = old;
    current.public_order_id = current.private_order_id = 9201;
    current.ext_id = 200;
    current.sess_id = 101;
    current.public_amount_rest = 1;
    current.public_action = 1;
    observe(manager, current);
    require(!manager.orders().contains("carry"), "old-session terminal order was never pruned");
    require(manager.orders().at("old-live").state == OrderState::Working &&
                manager.orders().at("old-unknown").state == OrderState::Unknown,
            "session pruning removed a carried live or unresolved order");
    f.session = 101;
    require(!manager.place(request("carry")).empty(), "pruning freed a previously used client identity");
    manager.on_reply(old_reply, {.msgid = 179, .order_id = 9999}, OrderManager::Clock::time_point{});
    manager.on_reply(old_cancel, {.msgid = 177}, OrderManager::Clock::time_point{});
    auto relist = old;
    relist.public_order_id = relist.private_order_id = 9101;
    relist.id_ord1 = 9001;
    relist.sess_id = 101;
    relist.public_amount_rest = 3;
    relist.public_action = 1;
    auto foreign = relist;
    foreign.client_code = "ABCD999";
    observe(manager, foreign);
    require(!manager.orders().contains("carry"), "foreign relist revived a pruned logical order");
    observe(manager, relist);
    require(manager.orders().at("carry").order_id == 9101 && manager.orders().at("carry").sess_id == 101 &&
                manager.orders().at("carry").state == OrderState::PendingCancel &&
                !manager.orders().contains("recovered:101:9101"),
            "split-commit id_ord1 relist lost its pruned logical identity");
    observe(manager, old);
    require(manager.orders().at("carry").order_id == 9101 && manager.orders().at("carry").remaining == 3,
            "late old-session deletion hijacked the revived order");
}
void cached_risk_move_races() {
    const auto working = [](Fixture& f, OrderManager& manager) {
        require(manager.place(request("first", 3)).empty(), "risk-race Add refused");
        f.poll(manager, 0);
        manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 1001}, OrderManager::Clock::time_point{});
    };
    Fixture queued;
    queued.config.risk.max_notional_scaled = 60'000'000;
    auto cancelled = queued.manager();
    working(queued, cancelled);
    require(cancelled.move("first", "120", 3).empty(), "queued risk-race Move refused");
    require(!cancelled.place(request("blocked", 3)).empty(), "queued Move did not reserve replacement exposure");
    require(cancelled.cancel("first").empty(), "queued Move cancellation refused");
    require(cancelled.place(request("fits-after-unsent", 3)).empty(),
            "discarded unsent Move did not release its reservation");

    Fixture flood;
    flood.config.risk.max_notional_scaled = 60'000'000;
    auto superseded = flood.manager();
    working(flood, superseded);
    require(superseded.move("first", "120", 3).empty(), "sent risk-race Move refused");
    flood.poll(superseded, 1);
    const auto move_id = flood.sent.back().id;
    require(superseded.cancel_all(42).empty(), "risk-race mass cancellation refused");
    require(!superseded.place(request("still-blocked", 3)).empty(),
            "mass cancel released a possibly accepted in-flight replacement");
    superseded.on_reply(move_id, {.msgid = 99, .penalty_remain = 1000},
                        OrderManager::Clock::time_point{} + std::chrono::milliseconds(2));
    require(superseded.place(request("fits-after-flood", 3)).empty(),
            "superseded Move99 did not release rejected replacement exposure");

    Fixture rejected;
    rejected.config.risk.max_notional_scaled = 60'000'000;
    auto settled = rejected.manager();
    working(rejected, settled);
    require(settled.move("first", "120", 3).empty(), "business-rejected Move refused locally");
    rejected.poll(settled, 1);
    settled.on_reply(rejected.sent.back().id, {.msgid = 176, .code = 17},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(2));
    require(settled.place(request("fits-after-reject", 3)).empty(), "rejected Move retained excess reservation");

    Fixture ambiguous;
    ambiguous.config.risk.max_notional_scaled = 75'000'000;
    auto unknown = ambiguous.manager();
    working(ambiguous, unknown);
    require(unknown.move("first", "110", 5).empty(), "ambiguous risk-race Move refused");
    ambiguous.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
    ambiguous.poll(unknown, 1);
    const auto uncertain_move = ambiguous.sent.at(1).id;
    require(!unknown.place(request("over-unknown", 3)).empty() && unknown.place(request("fits-unknown", 2)).empty(),
            "unknown replacement lost its conservative exposure reservation");
    unknown.on_timeout(uncertain_move, OrderManager::Clock::time_point{} + std::chrono::milliseconds(61001));
    observe(unknown, row(unknown.orders().at("first"), 1001, 0, 0));
    require(unknown.orders().at("first").state == OrderState::Unknown &&
                !unknown.place(request("after-old-delete", 1)).empty(),
            "old-ID deletion released an uncertain timed-out replacement reservation");
    unknown.on_reply(uncertain_move, {.msgid = 176, .order_id1 = 6001},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(61002));
    require(unknown.orders().at("first").order_id == 6001,
            "timed-out replacement lost the correlation needed for authoritative176");
    observe(unknown, row(unknown.orders().at("first"), 6001, 0, 0));
    require(unknown.place(request("after-replacement-terminal", 3)).empty(),
            "authoritative replacement termination did not release reserved risk");

    Fixture relisted;
    relisted.config.risk.max_notional_scaled = 60'000'000;
    auto next_day = relisted.manager();
    working(relisted, next_day);
    require(next_day.move("first", "120", 5).empty(), "day-boundary replacement refused");
    relisted.poll(next_day, 1);
    const auto old_move = relisted.sent.back().id;
    next_day.on_timeout(old_move, OrderManager::Clock::time_point{} + std::chrono::milliseconds(61001));
    auto linked = row(next_day.orders().at("first"), 7001, 3, 1, 101);
    linked.id_ord1 = 1001;
    observe(next_day, linked);
    next_day.on_reply(old_move, {.msgid = 176, .order_id1 = 6001},
                      OrderManager::Clock::time_point{} + std::chrono::milliseconds(61002));
    require(next_day.orders().at("first").order_id == 7001 && next_day.orders().at("first").sess_id == 101 &&
                next_day.orders().at("first").request.quantity == 3 &&
                next_day.orders().at("first").request.price == "100" &&
                next_day.orders().at("first").state == OrderState::PendingCancel,
            "late old-session176 rolled back a proven next-session relist");
    relisted.session = 101;
    require(next_day.place(request("fits-after-relist", 3)).empty(),
            "proven next-session relist retained old replacement exposure");
    next_day.on_timeout(old_move, OrderManager::Clock::time_point{} + std::chrono::milliseconds(62000));
    require(relisted.log.back().starts_with("late_timeout"), "proven relist retained an old Move indefinitely");

    Fixture overflow;
    overflow.config.risk.max_notional_scaled = INT64_MAX;
    auto excessive = overflow.manager();
    ManagedOrder seed{.request = request("huge", 1)};
    auto huge = row(seed, 8001, INT64_MAX, 1);
    huge.ext_id = 8001;
    observe(excessive, huge);
    auto another = huge;
    another.public_order_id = another.private_order_id = 8002;
    another.ext_id = 8002;
    observe(excessive, another);
    require(!excessive.place(request("over-cap", 1)).empty(), "large reconstructed exposure overflowed the cap");
    huge.public_amount_rest = 0;
    huge.public_action = 0;
    observe(excessive, huge);
    require(!excessive.place(request("still-over-cap", 1)).empty(),
            "subtracting one oversized order prematurely freed aggregate budget");
    another.public_amount_rest = 0;
    another.public_action = 0;
    observe(excessive, another);
    require(excessive.place(request("budget-restored", 1)).empty(),
            "two-word exposure total did not release terminated oversized orders");

    Fixture invalid;
    auto invalid_price = invalid.manager();
    auto malformed = row(seed, 8101, 1, 1);
    malformed.ext_id = 8101;
    malformed.price = "invalid";
    observe(invalid_price, malformed);
    require(!invalid_price.place(request("price-unavailable", 1)).empty(),
            "invalid recovered price did not block risk");
    malformed.public_amount_rest = 0;
    malformed.public_action = 0;
    observe(invalid_price, malformed);
    require(invalid_price.place(request("price-cleared", 1)).empty(),
            "terminated invalid price poisoned the risk cache");
}
void carried_fill_dedup_across_sessions() {
    for (const bool relist : {false, true}) {
        Fixture f;
        auto manager = f.manager();
        require(manager.place(request("filled-carry")).empty(), "filled carry Add refused");
        f.poll(manager, 0);
        manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 9401}, OrderManager::Clock::time_point{});
        fill(manager, 9401, 8401, 1);
        std::int64_t previous = 9401;
        for (std::int32_t session = 101; session <= 102; ++session) {
            auto next = row(manager.orders().at("filled-carry"), 9401 + session - 100, 2, 1, session);
            if (relist)
                next.id_ord1 = previous;
            else
                next.ext_id = session;
            observe(manager, next);
            previous = next.public_order_id;
        }
        fill(manager, 9401, 8401, 1);
        require(manager.orders().at("filled-carry").executed == 1,
                "session pruning discarded fill dedup for a retained carried order");
    }
}
void late_first_seen_carry_counts_for_risk() {
    Fixture f;
    f.config.risk.max_notional_scaled = 40'000'000;
    auto manager = f.manager();
    ManagedOrder seed{.request = request("new-session", 1)};
    for (std::int32_t session = 101; session <= 102; ++session) {
        auto newer = row(seed, 9500 + session, 1, 1, session);
        newer.ext_id = session;
        observe(manager, newer);
    }
    auto carry = row(seed, 9501, 3, 1, 100);
    carry.ext_id = 501;
    carry.public_amount = 3;
    observe(manager, carry);
    f.session = 102;
    require(manager.orders().contains("recovered:100:9501") && !manager.place(request("over-late-carry", 1)).empty(),
            "late first-seen authoritative carried exposure was ignored because of session age");
}
void unknown_flood_reply_still_paces_publisher() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("after-unknown-flood")).empty(), "unknown flood fixture Add refused");
    manager.on_reply(9999, {.msgid = 99, .penalty_remain = 2000}, OrderManager::Clock::time_point{});
    f.poll(manager, 1999);
    require(f.sent.empty(), "valid99 for expired/pruned correlation did not pace the shared publisher");
    f.poll(manager, 2000);
    require(f.sent.size() == 1 && !manager.operator_action_required(),
            "unknown99 spent a business retry or delayed past its penalty");
}
void official_add_reply_owns_identity() {
    Fixture f;
    f.config.risk.max_notional_scaled = 50'000'000;
    auto manager = f.manager();
    require(manager.place(request("official")).empty(), "official Add refused");
    f.poll(manager, 0);
    const auto add = f.sent.front().id;
    auto wrong = row(manager.orders().at("official"), 7777, 3, 1);
    observe(manager, wrong);
    fill(manager, 7777, 8701, 1);
    require(manager.orders().at("official").order_id == 0 && manager.orders().at("official").executed == 0,
            "pre179 ext collision took the Add identity or fills");
    require(!manager.place(request("held-risk", 1)).empty(), "held own row exposure was omitted");
    require(manager.cancel("official").empty(), "unresolved Add cancel refused");
    f.poll(manager, 1);
    require(f.sent.back().kind == tr::Plaza2TradeCommandKind::DelUserOrders,
            "pre179 cancellation targeted a provisional foreign ID");
    manager.on_reply(add, {.msgid = 179, .order_id = 63011},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(2));
    f.poll(manager, 3);
    require(manager.orders().at("official").order_id == 63011 && manager.orders().at("official").executed == 0 &&
                wire<official_cgate99::DelOrder>(f.sent.back()).order_id == 63011,
            "official179 did not win or cancellation retained foreign identity");
    require(!manager.place(request("post179-risk", 1)).empty(),
            "official179 transfer omitted one of the two owned active identities");
    fill(manager, 7777, 8702, 1);
    require(manager.orders().at("official").executed == 0 && manager.orders().at("recovered:100:7777").executed == 2,
            "foreign own fills were lost or charged to the official Add");
    wrong.public_amount_rest = 0;
    wrong.public_action = 0;
    observe(manager, wrong);
    require(manager.place(request("held-risk-released", 1)).empty(), "terminated held exposure poisoned risk");
}
void late_add_reply_after_absence_is_still_authoritative() {
    Fixture f;
    f.certainty = cg::Plaza2SubmissionCertainty::PossiblySent;
    f.config.risk.max_notional_scaled = 50'000'000;
    auto manager = f.manager();
    require(manager.place(request("late-absence")).empty(), "late absence Add refused");
    f.poll(manager, 0);
    const auto add_id = f.sent.front().id;
    f.certainty = cg::Plaza2SubmissionCertainty::Posted;
    f.poll(manager, 1000);
    require(f.sent.back().kind == tr::Plaza2TradeCommandKind::DelUserOrders, "uncertain Add did not use ext recovery");
    manager.on_reply(f.sent.back().id, {.msgid = 186, .num_orders = 0},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(1000));
    manager.prove_absence(1700000062, true);
    require(manager.orders().at("late-absence").state == OrderState::Cancelled,
            "late absence fixture did not obtain an absence resolution");
    ManagedOrder unrelated{.request = request("unrelated", 1)};
    unrelated.request.isin_id = 43;
    observe(manager, row(unrelated, 9921, 1, 1, 101));
    observe(manager, row(unrelated, 9922, 1, 1, 102));
    f.poll(manager, 70000);
    manager.on_reply(add_id, {.msgid = 179, .order_id = 63027},
                     OrderManager::Clock::time_point{} + std::chrono::milliseconds(70001));
    require(manager.orders().at("late-absence").order_id == 63027 &&
                manager.orders().at("late-absence").remaining == 3 &&
                manager.orders().at("late-absence").cancel_requested,
            "late179 acceptance was discarded by absence or session pruning");
    require(!manager.place(request("late-acceptance-risk", 1)).empty(),
            "late179 acceptance failed to restore known outstanding exposure");
    f.poll(manager, 71000);
    require(std::any_of(f.sent.begin(), f.sent.end(),
                        [](const auto& sent) {
                            return sent.kind == tr::Plaza2TradeCommandKind::DelOrder &&
                                   wire<official_cgate99::DelOrder>(sent).order_id == 63027;
                        }),
            "late accepted identity was not directly cancelled");
    require(std::count_if(f.sent.begin(), f.sent.end(),
                          [](const auto& sent) { return sent.kind == tr::Plaza2TradeCommandKind::AddOrder; }) == 1,
            "late179 recovery blindly resubmitted Add");
}
void rejected_add_preserves_owned_evidence() {
    for (const bool flood : {false, true}) {
        Fixture f;
        f.config.risk.max_notional_scaled = 50'000'000;
        auto manager = f.manager();
        require(manager.place(request("rejected")).empty(), "rejected Add fixture refused");
        f.poll(manager, 0);
        auto collision = row(manager.orders().at("rejected"), 7788, 3, 1);
        observe(manager, collision);
        fill(manager, 7788, 8703, 1);
        manager.on_reply(f.sent.front().id,
                         flood ? tr::Plaza2TradeDecodedReply{.msgid = 99, .penalty_remain = 1}
                               : tr::Plaza2TradeDecodedReply{.msgid = 179, .code = 5},
                         OrderManager::Clock::time_point{});
        require(manager.orders().at("rejected").state == OrderState::Rejected &&
                    manager.orders().at("recovered:100:7788").executed == 1,
                "definitive Add rejection discarded actual owned identity or fill");
        require(!manager.place(request("rejection-risk", 3)).empty(),
                "Add rejection removed colliding owned account exposure");
        collision.public_amount_rest = 0;
        collision.public_action = 0;
        observe(manager, collision);
        require(manager.place(request("after-rejection", 3)).empty(),
                "terminated colliding identity retained a held risk charge");
    }
}
void official_add_terminal_and_old_fill_evidence() {
    Fixture held;
    auto manager = held.manager();
    require(manager.place(request("held-terminal")).empty(), "terminal Add fixture refused");
    held.poll(manager, 0);
    const auto actual = row(manager.orders().at("held-terminal"), 63021, 0, 0);
    observe(manager, actual);
    fill(manager, 63021, 8801, 1);
    manager.on_reply(held.sent.front().id, {.msgid = 179, .order_id = 63021}, OrderManager::Clock::time_point{});
    require(manager.orders().at("held-terminal").state == OrderState::Cancelled &&
                manager.orders().at("held-terminal").remaining == 0 &&
                manager.orders().at("held-terminal").executed == 1,
            "official179 resurrected an already-terminal exact native identity");

    Fixture historical;
    auto retained = historical.manager();
    require(retained.place(request("historical-fill")).empty(), "historical fill Add refused");
    historical.poll(retained, 0);
    auto old = row(retained.orders().at("historical-fill"), 63022, 2, 1);
    observe(retained, old);
    fill(retained, 63022, 8802, 1);
    ManagedOrder unrelated{.request = request("unrelated", 1)};
    unrelated.request.isin_id = 43;
    observe(retained, row(unrelated, 9901, 1, 1, 101));
    observe(retained, row(unrelated, 9902, 1, 1, 102));
    retained.on_reply(historical.sent.front().id, {.msgid = 179, .order_id = 63022}, OrderManager::Clock::time_point{});
    require(retained.orders().at("historical-fill").executed == 1 &&
                retained.orders().at("historical-fill").remaining == 2,
            "session pruning lost held actual-ID fill evidence before late179");
    fill(retained, 63022, 8802, 1);
    require(retained.orders().at("historical-fill").executed == 1, "held fill replay charged the same deal twice");

    for (const bool terminal_only : {false, true}) {
        Fixture f;
        f.config.risk.max_notional_scaled = 50'000'000;
        auto exact = f.manager();
        require(exact.place(request("ext-zero")).empty(), "ext0 Add fixture refused");
        f.poll(exact, 0);
        auto recovered = row(exact.orders().at("ext-zero"), 63023, terminal_only ? 0 : 2, terminal_only ? 0 : 1);
        recovered.ext_id = 0;
        observe(exact, recovered);
        fill(exact, 63023, 8803, 1);
        if (!terminal_only) {
            recovered.public_amount_rest = 0;
            recovered.public_action = 0;
            observe(exact, recovered);
        }
        observe(exact, row(unrelated, 9911, 1, 1, 101));
        observe(exact, row(unrelated, 9912, 1, 1, 102));
        require(!exact.orders().contains("recovered:100:63023"), "old terminal recovered object was not pruned");
        exact.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 63023}, OrderManager::Clock::time_point{});
        const auto final = std::find_if(f.log.rbegin(), f.log.rend(), [](const auto& line) {
            return line.starts_with("order") && line.find("\"client_order_id\":\"ext-zero\"") != std::string::npos;
        });
        require(final != f.log.rend() && final->find("\"state\":\"Cancelled\"") != std::string::npos &&
                    final->find("\"executed\":1") != std::string::npos &&
                    (!exact.orders().contains("ext-zero") || exact.orders().at("ext-zero").remaining == 0),
                "late179 lost pruned ext0 native terminal/fill evidence");
        require(exact.place(request("after-terminal-evidence", 3)).empty(),
                "late179 created phantom exposure for a terminal owned ID");
        require(std::none_of(f.sent.begin(), f.sent.end(),
                             [](const auto& send) { return send.kind == tr::Plaza2TradeCommandKind::DelOrder; }),
                "late179 directly cancelled a known terminal native identity");
    }
}
void official_add_merges_recovered_and_claims_other_ext() {
    Fixture f;
    f.config.risk.max_notional_scaled = 50'000'000;
    auto manager = f.manager();
    require(manager.place(request("merge")).empty(), "recovered merge Add refused");
    f.poll(manager, 0);
    auto actual = row(manager.orders().at("merge"), 63024, 2, 1);
    actual.ext_id = 0;
    observe(manager, actual);
    fill(manager, 63024, 8804, 1);
    require(manager.orders().at("recovered:100:63024").executed == 1,
            "ext0 native identity did not reconstruct actual fills");
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 63024}, OrderManager::Clock::time_point{});
    require(manager.orders().at("merge").executed == 1 && manager.orders().at("merge").remaining == 2 &&
                !manager.orders().contains("recovered:100:63024"),
            "official179 failed to merge recovered exact identity/fills");
    fill(manager, 63024, 8804, 1);
    require(manager.orders().at("merge").executed == 1 && manager.place(request("after-merge", 3)).empty(),
            "recovered merge duplicated fill or risk charge");

    Fixture concurrent;
    auto two = concurrent.manager();
    require(two.place(request("A")).empty() && two.place(request("B")).empty(), "concurrent Adds refused");
    concurrent.poll(two, 0);
    auto held_by_a = row(two.orders().at("A"), 63025, 2, 1);
    observe(two, held_by_a);
    fill(two, 63025, 8805, 1);
    two.on_reply(concurrent.sent.at(1).id, {.msgid = 179, .order_id = 63025}, OrderManager::Clock::time_point{});
    require(two.orders().at("B").order_id == 63025 && two.orders().at("B").executed == 1 &&
                two.orders().at("A").order_id == 0,
            "official179 could not claim identity held under another provisional ext_id");
    held_by_a.public_amount_rest = 0;
    held_by_a.public_action = 0;
    observe(two, held_by_a);
    require(two.orders().at("B").state == OrderState::Cancelled && two.orders().at("A").order_id == 0,
            "other Add's provisional hold swallowed an officially-owned terminal update");
    two.on_reply(concurrent.sent.front().id, {.msgid = 179, .order_id = 63026}, OrderManager::Clock::time_point{});
    require(two.orders().at("A").order_id == 63026 && two.orders().at("A").executed == 0 &&
                two.orders().at("B").executed == 1,
            "resolving the other Add reassigned official identity or historical fills");
}
void official_add_follows_relist_without_ancestor_row() {
    Fixture f;
    f.config.risk.max_notional_scaled = 50'000'000;
    auto manager = f.manager();
    require(manager.place(request("missing-root")).empty(), "missing root Add refused");
    f.poll(manager, 0);
    fill(manager, 9001, 8901, 1);
    auto child = row(manager.orders().at("missing-root"), 9101, 1, 1, 101);
    child.ext_id = 0;
    child.id_ord1 = 9001;
    observe(manager, child);
    fill(manager, 9101, 8902, 1, 101);
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 9001}, OrderManager::Clock::time_point{});
    require(manager.orders().at("missing-root").order_id == 9101 &&
                manager.orders().at("missing-root").sess_id == 101 &&
                manager.orders().at("missing-root").executed == 2 &&
                manager.orders().at("missing-root").remaining == 1 && !manager.orders().contains("recovered:101:9101"),
            "official179 failed to follow explicit relist whose ancestor row was absent");
    fill(manager, 9001, 8901, 1);
    fill(manager, 9101, 8902, 1, 101);
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 9001}, OrderManager::Clock::time_point{});
    require(manager.orders().at("missing-root").executed == 2 && manager.orders().at("missing-root").order_id == 9101 &&
                manager.place(request("missing-root-risk", 4)).empty(),
            "missing-root relist replay duplicated fills/risk or rolled identity back");

    Fixture multiple;
    multiple.config.risk.max_notional_scaled = 100'000'000;
    auto pending = multiple.manager();
    require(pending.place(request("older")).empty(), "older candidate Add refused");
    multiple.poll(pending, 0);
    const auto older = multiple.sent.front().id;
    multiple.session = 101;
    require(pending.place(request("later")).empty(), "later candidate Add refused");
    multiple.poll(pending, 1000);
    const auto later = multiple.sent.back().id;
    fill(pending, 9201, 8903, 1, 101);
    auto linked = row(pending.orders().at("later"), 9301, 1, 1, 103);
    linked.ext_id = 0;
    linked.id_ord1 = 9201;
    observe(pending, linked);
    fill(pending, 9301, 8904, 1, 103);
    pending.on_reply(older, {.msgid = 179, .code = 5}, OrderManager::Clock::time_point{});
    linked.public_amount_rest = 0;
    linked.public_action = 0;
    observe(pending, linked);
    ManagedOrder unrelated{.request = request("unrelated", 1)};
    unrelated.request.isin_id = 43;
    observe(pending, row(unrelated, 9931, 1, 1, 104));
    observe(pending, row(unrelated, 9932, 1, 1, 105));
    require(!pending.orders().contains("recovered:103:9301"), "old terminal descendant was not pruned");
    pending.on_reply(later, {.msgid = 179, .order_id = 9201}, OrderManager::Clock::time_point{});
    const auto final = std::find_if(multiple.log.rbegin(), multiple.log.rend(), [](const auto& line) {
        return line.starts_with("order") && line.find("\"client_order_id\":\"later\"") != std::string::npos;
    });
    require(final != multiple.log.rend() && final->find("\"state\":\"Cancelled\"") != std::string::npos &&
                final->find("\"executed\":2") != std::string::npos,
            "resolving one Add scope discarded missing-root evidence/fills needed by another Add");
}
void delayed_relisted_fill_survives_raw_candidate_pruning() {
    Fixture f;
    auto manager = f.manager();
    require(manager.place(request("delayed-child-fill")).empty(), "delayed child Add refused");
    f.poll(manager, 0);
    auto child = row(manager.orders().at("delayed-child-fill"), 9102, 0, 0, 101);
    child.ext_id = 0;
    child.id_ord1 = 9002;
    observe(manager, child);
    ManagedOrder unrelated{.request = request("unrelated", 1)};
    unrelated.request.isin_id = 43;
    observe(manager, row(unrelated, 9941, 1, 1, 102));
    observe(manager, row(unrelated, 9942, 1, 1, 103));
    require(!manager.orders().contains("recovered:101:9102"), "terminal child was unexpectedly retained as an object");
    fill(manager, 9102, 8905, 1, 101); // First deal arrives after terminal history was pruned.
    observe(manager, row(unrelated, 9943, 1, 1, 104));
    manager.on_reply(f.sent.front().id, {.msgid = 179, .order_id = 9002}, OrderManager::Clock::time_point{});
    const auto final = std::find_if(f.log.rbegin(), f.log.rend(), [](const auto& line) {
        return line.starts_with("order") &&
               line.find("\"client_order_id\":\"delayed-child-fill\"") != std::string::npos;
    });
    require(final != f.log.rend() && final->find("\"state\":\"Cancelled\"") != std::string::npos &&
                final->find("\"executed\":1") != std::string::npos,
            "session pruning discarded delayed fill for retained raw id_ord1 candidate");
    const auto credited = std::count_if(f.log.begin(), f.log.end(), [](const auto& line) {
        return line.starts_with("trade") && line.find("\"deal_id\":8905") != std::string::npos;
    });
    fill(manager, 9102, 8905, 1, 101);
    require(credited == 1 && std::count_if(f.log.begin(), f.log.end(),
                                           [](const auto& line) {
                                               return line.starts_with("trade") &&
                                                      line.find("\"deal_id\":8905") != std::string::npos;
                                           }) == 1,
            "delayed terminal child fill replay charged the same deal twice");
}
void manager_scale() {
    for (const std::size_t count : {1000U, 150000U}) {
        OrderManagerConfig config;
        config.broker_code = "ABCD";
        config.client_code = "001";
        config.risk.max_open_orders = 200000;
        config.risk.max_notional_scaled = INT64_MAX;
        Fixture terms;
        OrderManager manager(
            config,
            [](const auto&, auto) {
                return cg::Plaza2PublisherMessageResult{.certainty = cg::Plaza2SubmissionCertainty::PossiblySent};
            },
            [](auto) { return true; }, [&](auto isin) { return terms.terms(isin); });
        std::vector<ps::OwnOrderSnapshot> snapshot;
        snapshot.reserve(count);
        ManagedOrder seed{.request = request("scale", 1)};
        for (std::size_t i = 0; i < count; ++i) {
            auto current = row(seed, static_cast<std::int64_t>(i + 1), 1, 1);
            current.ext_id = static_cast<std::int32_t>(i + 1);
            snapshot.push_back(std::move(current));
        }
        manager.observe_orders(snapshot, true);
        require(manager.place(request("scale-unknown", 1)).empty(), "scale unresolved Add refused");
        manager.poll(OrderManager::Clock::time_point{}, 1700000000);
        const auto start = OrderManager::Clock::now();
        for (int poll = 0; poll < 1000; ++poll)
            manager.prove_absence(1800000000, true);
        require(manager.orders().at("scale-unknown").state == OrderState::Unknown,
                "absence poll guessed an unconfirmed uncertain Add away");
        const auto proof_end = OrderManager::Clock::now();
        for (int add = 0; add < 128; ++add)
            require(manager.place(request("scale-add-" + std::to_string(add), 1)).empty(),
                    "scale workload rejected a valid under-cap Add");
        const auto elapsed = OrderManager::Clock::now() - start;
        const auto proof_us = std::chrono::duration_cast<std::chrono::microseconds>(proof_end - start).count();
        const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        std::cout << "manager scale " << count << ": 1000 absence polls=" << proof_us
                  << "us, plus 128 accepted Adds total=" << total_ms << "ms\n";
        require(elapsed < std::chrono::milliseconds(500), "manager polling/admission still scales with all orders");
    }
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
        rejected_mass_cancel_falls_back_to_each_known_id();
        mass_cancel_supersedes_delayed_flood_replies();
        cancel_all_and_move_failures();
        uncertain_cancels_preserve_identity_and_budget();
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
        terminal_pruning_and_relist();
        cached_risk_move_races();
        carried_fill_dedup_across_sessions();
        late_first_seen_carry_counts_for_risk();
        unknown_flood_reply_still_paces_publisher();
        official_add_reply_owns_identity();
        late_add_reply_after_absence_is_still_authoritative();
        rejected_add_preserves_owned_evidence();
        official_add_terminal_and_old_fill_evidence();
        official_add_merges_recovered_and_claims_other_ext();
        official_add_follows_relist_without_ancestor_row();
        delayed_relisted_fill_survives_raw_candidate_pruning();
        manager_scale();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
