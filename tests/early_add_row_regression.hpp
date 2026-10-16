#pragma once

#include "fixtures/cgate99_messages.hpp"
#include "moex/connector_host/order_manager.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace moex::connector_host::early_add_row_detail {
namespace ps = plaza2::private_state;
namespace cg = plaza2::cgate;
namespace tr = plaza2_trade;

inline void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

struct Sent {
    tr::Plaza2TradeCommandKind kind;
    std::uint32_t id;
    std::vector<std::byte> payload;
};

// A real OrderManager with a deterministic owner-loop clock and a current,
// authoritative zero POS. The sender records actual manager submissions.
struct Fixture {
    OrderManagerConfig config;
    std::vector<Sent> sent;
    std::vector<std::string> log;
    std::int64_t ms{};
    std::int32_t session{100};
    Fixture() {
        config.broker_code = "ABCD";
        config.client_code = "001";
        config.login_from = "owner-login";
        config.ext_id_range_configured = true;
        config.ext_id_begin = config.next_ext_id = 100;
        config.ext_id_end = 199;
        config.max_commands_per_second = 5;
        config.reply_timeout = std::chrono::seconds(60);
        config.risk.max_quantity = 2;
        config.risk.max_open_orders = 2;
        config.risk.max_notional_scaled = 20'000'000;
        config.risk.max_notional_by_isin = {{42, 20'000'000}};
        config.risk.max_position_by_isin = {{42, 2}};
    }
    static OrderManager::Clock::time_point time(std::int64_t value) {
        return OrderManager::Clock::time_point{} + std::chrono::milliseconds(value);
    }
    OrderManager manager() {
        return OrderManager(
            config,
            [this](const auto& command, auto id) {
                sent.push_back({command.command_kind, id, command.payload});
                return cg::Plaza2PublisherMessageResult{.certainty = cg::Plaza2SubmissionCertainty::Posted,
                                                        .post_invoked = true};
            },
            [](auto) { return true; },
            [this](auto isin) -> std::optional<ps::FutureSessionTerms> {
                ps::FutureSessionTerms terms;
                terms.isin_id = isin;
                terms.sess_id = session;
                terms.min_step = ps::SessionDecimal{100000};
                terms.bounds.lower = ps::SessionDecimal{100000};
                terms.bounds.upper = ps::SessionDecimal{100000000};
                terms.bounds.interval_valid = true;
                return terms;
            },
            [this](auto kind, auto fields) { log.push_back(std::string(kind) + std::string(fields)); },
            [](auto) -> std::optional<std::int64_t> { return 0; },
            [](auto) -> std::optional<PositionProof> {
                return PositionProof{.trade_lifenum = 7, .calendar_revision = 100};
            });
    }
    void poll(OrderManager& manager, std::int64_t value) {
        ms = value;
        manager.poll(time(value), 1700000000 + value / 1000);
    }
};

inline OrderRequest add(std::string key, std::int32_t quantity = 1) {
    return {.client_order_id = std::move(key), .isin_id = 42, .price = "100", .quantity = quantity};
}

inline ps::OwnOrderSnapshot own(const ManagedOrder& order, std::int64_t id = 64101) {
    ps::OwnOrderSnapshot row;
    row.public_order_id = row.private_order_id = id;
    row.sess_id = 100;
    row.isin_id = 42;
    row.client_code = "ABCD001";
    row.login_from = "owner-login";
    row.price = "100.00000"; // Numeric equality, not submitted string equality.
    row.public_amount = row.public_amount_rest = order.request.quantity;
    row.public_action = row.dir = 1;
    row.ext_id = order.ext_id;
    row.from_trade_repl = row.from_current_day = true;
    row.trade_repl_commit_sequence = 1;
    return row;
}

inline void observe(OrderManager& manager, const ps::OwnOrderSnapshot& row, bool rebuilding = false) {
    manager.observe_orders(std::span(&row, 1), rebuilding);
}

inline ps::OwnTradeSnapshot fill(std::int64_t order_id, std::int64_t deal_id = 84101) {
    ps::OwnTradeSnapshot row;
    row.id_deal = deal_id;
    row.sess_id = 100;
    row.isin_id = 42;
    row.price = "100.00000";
    row.amount = 1;
    row.public_order_id_buy = row.private_order_id_buy = order_id;
    row.code_buy = "ABCD001";
    row.login_buy = "owner-login";
    row.repl_rev = 101;
    row.trade_lifenum = 7;
    return row;
}

inline void observe(OrderManager& manager, const ps::OwnTradeSnapshot& row) {
    manager.observe_trades(std::span(&row, 1));
}

template <class Wire> inline Wire wire(const Sent& sent) {
    check(sent.payload.size() == sizeof(Wire), "early Add regression native wire layout mismatch");
    Wire result{};
    std::memcpy(&result, sent.payload.data(), sizeof(result));
    return result;
}

inline void positive_tight_risk_and_reply() {
    Fixture fixture;
    auto manager = fixture.manager();
    check(manager.place(add("early-first")).empty(), "early Add seed refused");
    fixture.poll(manager, 0);
    const auto first_uid = fixture.sent.front().id;
    const auto row = own(manager.orders().at("early-first"));
    fixture.poll(manager, 20);
    observe(manager, row);
    const auto& early = manager.orders().at("early-first");
    check(early.order_id == 64101 && early.confirmed_by_replication && early.add_unconfirmed && early.instance_owned &&
              early.remaining == 1 && early.executed == 0,
          "exact committed row did not bind the posted Add while retaining its179 confirmation");
    check(manager.orders().size() == 1 && !manager.orders().contains("recovered:100:64101"),
          "exact early Add row created a recovered alias/double reservation");
    check(!manager.move("early-first", "101", 1).empty(), "early row bypassed pending Add's Move guard");
    check(manager.place(add("early-second")).empty(),
          "20ms-early native row doubled position/notional/open-order risk before179");
    check(!manager.place(add("over-tight-cap")).empty(), "early merge released actual risk or enlarged a cap");
    fixture.poll(manager, 21);
    check(fixture.sent.size() == 2 && fixture.sent.back().kind == tr::Plaza2TradeCommandKind::AddOrder,
          "second same-ISIN Add did not post before first179");
    const auto second_uid = fixture.sent.back().id;
    fixture.poll(manager, 40);
    manager.on_reply(first_uid, {.msgid = 179, .order_id = 64101}, Fixture::time(40));
    manager.on_reply(second_uid, {.msgid = 179, .order_id = 64102}, Fixture::time(40));
    observe(manager, row);
    manager.on_reply(first_uid, {.msgid = 179, .order_id = 64101}, Fixture::time(41));
    check(manager.orders().size() == 2 && manager.orders().at("early-first").order_id == 64101 &&
              !manager.orders().at("early-first").add_unconfirmed &&
              !manager.orders().at("early-first").operator_action_required,
          "matching/duplicate179 changed the exact row identity or left it unconfirmed");
    check(!manager.place(add("still-tight-after179")).empty(), "matching179 dropped one actual exposure");
}

inline void committed_fill_dedup_and_terminal() {
    Fixture fixture;
    auto manager = fixture.manager();
    check(manager.place(add("early-filled", 2)).empty(), "early fill seed refused");
    fixture.poll(manager, 0);
    const auto uid = fixture.sent.front().id;
    auto row = own(manager.orders().at("early-filled"), 64103);
    row.public_amount_rest = 1;
    fixture.poll(manager, 20);
    observe(manager, row);
    const auto deal = fill(64103);
    observe(manager, deal);
    observe(manager, row);
    observe(manager, deal);
    check(manager.orders().at("early-filled").order_id == 64103 && manager.orders().at("early-filled").executed == 1 &&
              manager.orders().at("early-filled").remaining == 1,
          "committed early row/fill failed exact-once credit before179");
    const auto before = manager.pending_fill_reservations(42);
    check(before.buy_quantity == 1 && !before.buy_overflow, "early fill duplicated its POS catch-up reservation");
    fixture.poll(manager, 40);
    manager.on_reply(uid, {.msgid = 179, .order_id = 64103}, Fixture::time(40));
    observe(manager, deal);
    manager.on_reply(uid, {.msgid = 179, .order_id = 64103}, Fixture::time(41));
    check(manager.orders().at("early-filled").executed == 1 && manager.orders().at("early-filled").remaining == 1 &&
              manager.pending_fill_reservations(42).buy_quantity == 1 &&
              !manager.orders().contains("recovered:100:64103"),
          "late179 or replayed early native evidence duplicated fill/position risk");
    row.public_amount_rest = row.public_action = 0;
    row.trade_repl_commit_sequence = 2;
    observe(manager, row);
    manager.on_reply(uid, {.msgid = 179, .order_id = 64103}, Fixture::time(42));
    check(manager.orders().at("early-filled").state == OrderState::Cancelled &&
              manager.orders().at("early-filled").executed == 1 && manager.orders().at("early-filled").remaining == 0,
          "late179 resurrected a terminated early-bound order");

    Fixture before_reply;
    auto terminal = before_reply.manager();
    check(terminal.place(add("terminal-before179")).empty(), "early terminal seed refused");
    before_reply.poll(terminal, 0);
    auto final = own(terminal.orders().at("terminal-before179"), 64104);
    final.public_amount_rest = final.public_action = 0;
    before_reply.poll(terminal, 20);
    observe(terminal, final);
    terminal.on_reply(before_reply.sent.front().id, {.msgid = 179, .order_id = 64104}, Fixture::time(40));
    check(terminal.orders().at("terminal-before179").state == OrderState::Cancelled &&
              terminal.orders().at("terminal-before179").remaining == 0,
          "matching179 resurrected an early committed terminal order");
}

inline void provisional_cancel_uses_proven_identity() {
    Fixture fixture;
    auto manager = fixture.manager();
    check(manager.place(add("early-cancel")).empty(), "early cancel seed refused");
    fixture.poll(manager, 0);
    const auto add_uid = fixture.sent.front().id;
    auto row = own(manager.orders().at("early-cancel"), 64105);
    fixture.poll(manager, 20);
    observe(manager, row);
    check(manager.cancel("early-cancel").empty(), "early proven-ID cancellation refused");
    fixture.poll(manager, 21);
    check(fixture.sent.size() == 2 && fixture.sent.back().kind == tr::Plaza2TradeCommandKind::DelOrder &&
              wire<official_cgate99::DelOrder>(fixture.sent.back()).order_id == 64105,
          "early cancellation used an unknown ID, an Add retry, or an unscoped mass cancellation");
    manager.on_reply(fixture.sent.back().id, {.msgid = 177, .amount = 1}, Fixture::time(22), 1);
    check(manager.cancellations_pending(), "cancel reply alone falsely proved early order terminal");
    row.public_amount_rest = row.public_action = 0;
    row.trade_repl_commit_sequence = 2;
    observe(manager, row);
    manager.observe_trade_commit(2);
    manager.on_reply(add_uid, {.msgid = 179, .order_id = 64105}, Fixture::time(40));
    check(manager.orders().at("early-cancel").state == OrderState::Cancelled &&
              manager.orders().at("early-cancel").remaining == 0 && !manager.cancellations_pending(),
          "late179 lost an early cancellation's committed terminal result");
    check(std::count_if(fixture.sent.begin(), fixture.sent.end(),
                        [](const auto& sent) { return sent.kind == tr::Plaza2TradeCommandKind::AddOrder; }) == 1,
          "early cancellation recovery blindly retried Add");
}

inline void conservative_mismatches() {
    for (int mismatch = 0; mismatch < 22; ++mismatch) {
        Fixture fixture;
        if (mismatch == 14)
            fixture.config.ext_id_range_configured = false;
        if (mismatch == 15)
            fixture.config.login_from.clear();
        auto manager = fixture.manager();
        check(manager.place(add("must-not-merge")).empty(), "early mismatch seed refused");
        fixture.poll(manager, 0);
        auto row = own(manager.orders().at("must-not-merge"), 64200 + mismatch);
        switch (mismatch) {
        case 0:
            row.login_from = "foreign-login";
            break;
        case 1:
            row.client_code = "OTHER01";
            break;
        case 2:
            row.login_from.clear();
            break;
        case 3:
            row.isin_id = 43;
            break;
        case 4:
            row.dir = 2;
            break;
        case 5:
            row.price = "101";
            break;
        case 6:
            row.public_amount = 2;
            break;
        case 7:
            row.sess_id = 101;
            break;
        case 8:
            row.ext_id = 0;
            break;
        case 9:
            row.identity_conflict = true;
            break;
        case 10:
            row.multileg = true;
            break;
        case 11:
            row.from_trade_repl = false;
            row.from_user_book = true;
            break;
        case 12:
            row.trade_repl_commit_sequence = 0;
            break;
        case 13:
            row.ext_id = 200;
            break;
        case 14:
            break; // No assigned instance range.
        case 15:
            break; // Configured login is absent.
        case 16:
            row.public_amount = 0;
            break;
        case 17:
            row.id_ord1 = 64100;
            break;
        case 18:
            row.prevorder_id = 64100;
            break;
        case 19:
            row.sess_id = 99;
            break;
        case 20:
            row.from_trade_repl = false;
            break; // Current-day provenance alone is not committed TRADE.
        case 21:
            row.trade_repl_commit_sequence = UINT64_MAX;
            break; // Projector's in-flight transaction sentinel.
        }
        fixture.poll(manager, 20);
        observe(manager, row);
        const auto& original = manager.orders().at("must-not-merge");
        check(original.order_id == 0 && original.add_unconfirmed && original.executed == 0,
              "unproved early row assumed the pending Add's identity");
        check(!manager.place(add("original-risk-retained", 2)).empty(),
              "unproved early row released the original Add's full risk reservation");
        check(fixture.sent.size() == 1 && fixture.sent.front().kind == tr::Plaza2TradeCommandKind::AddOrder,
              "mismatched early row triggered a trading command");
    }
}

inline void queued_unsent_never_binds() {
    Fixture fixture;
    auto manager = fixture.manager();
    check(manager.place(add("not-posted")).empty(), "queued early Add fixture refused");
    const auto row = own(manager.orders().at("not-posted"), 64301);
    observe(manager, row);
    check(manager.orders().at("not-posted").order_id == 0 && !manager.orders().at("not-posted").add_unconfirmed &&
              fixture.sent.empty() && manager.queued() == 1,
          "an ext_id collision bound a queued Add that was never submitted");
    check(!manager.place(add("queued-risk-retained", 2)).empty(), "unsent Add's reservation disappeared");
}

inline void contradictory_reply_preserves_committed_exposure() {
    for (const int reply_kind : {0, 1, 2}) {
        Fixture fixture;
        auto manager = fixture.manager();
        check(manager.place(add("proven-before-reply")).empty(), "early conflict seed refused");
        fixture.poll(manager, 0);
        const auto uid = fixture.sent.front().id;
        const auto row = own(manager.orders().at("proven-before-reply"), 64302);
        fixture.poll(manager, 20);
        observe(manager, row);
        const auto reply = reply_kind == 0   ? tr::Plaza2TradeDecodedReply{.msgid = 179, .order_id = 64303}
                           : reply_kind == 1 ? tr::Plaza2TradeDecodedReply{.msgid = 179, .code = 5}
                                             : tr::Plaza2TradeDecodedReply{.msgid = 99, .penalty_remain = 2000};
        manager.on_reply(uid, reply, Fixture::time(40));
        const auto& order = manager.orders().at("proven-before-reply");
        check(order.order_id == 64302 && order.remaining == 1 && !terminal(order.state) &&
                  manager.operator_action_required(),
              "conflicting/rejecting179 discarded exact committed working exposure");
        check(!manager.place(add("must-stop-after-conflict")).empty(),
              "contradictory Add confirmation allowed new exposure");
        check(fixture.sent.size() == 1, "contradictory Add confirmation automatically retried a command");
    }
}
inline void order_row_before_deal_keeps_position_risk() {
    for (const std::int64_t remaining : {0, 1}) {
        Fixture fixture;
        auto manager = fixture.manager();
        check(manager.place(add("early-before-deal", 2)).empty(), "row-before-deal seed refused");
        fixture.poll(manager, 0);
        const auto uid = fixture.sent.front().id;
        auto row = own(manager.orders().at("early-before-deal"), 64304 + remaining);
        row.public_amount_rest = remaining;
        row.public_action = remaining ? 1 : 2;
        fixture.poll(manager, 20);
        observe(manager, row);
        check(manager.orders().at("early-before-deal").executed == 0 &&
                  manager.pending_fill_reservations(42).buy_quantity == 0,
              "early order row fabricated a fill or POS credit");
        check(!manager.place(add("must-retain-uncredited-risk")).empty(),
              "early reduced balance released uncredited execution risk");
        manager.on_reply(uid, {.msgid = 179, .order_id = row.public_order_id}, Fixture::time(40));
        check(!manager.place(add("179-does-not-prove-deal")).empty(), "179 released the missing user_deal reservation");
        if (remaining) {
            check(!manager.move("early-before-deal", "100", 2).empty(),
                  "Move ignored a committed execution without its deal baseline");
            row.public_amount_rest = row.public_action = 0;
            row.trade_repl_commit_sequence = 2;
            observe(manager, row);
            check(!manager.place(add("cancel-does-not-prove-deal", 2)).empty(),
                  "cancelling the remainder released already executed quantity");
        }
        auto actual = fill(row.public_order_id, 84304 + remaining);
        actual.amount = 2 - remaining;
        observe(manager, actual);
        observe(manager, actual);
        check(manager.orders().at("early-before-deal").executed == actual.amount &&
                  manager.pending_fill_reservations(42).buy_quantity == static_cast<std::uint64_t>(actual.amount),
              "actual user_deal did not replace the conservative hold exactly once");
        if (remaining)
            check(manager.place(add("one-real-fill-headroom")).empty(),
                  "credited one-lot deal retained a duplicate conservative hold");
        else
            check(!manager.place(add("two-real-fills-fill-cap")).empty(),
                  "credited two-lot fill was omitted before POS catch-up");
    }
}
} // namespace moex::connector_host::early_add_row_detail

namespace moex::connector_host {
inline void early_add_row_regression() {
    early_add_row_detail::positive_tight_risk_and_reply();
    early_add_row_detail::committed_fill_dedup_and_terminal();
    early_add_row_detail::provisional_cancel_uses_proven_identity();
    early_add_row_detail::conservative_mismatches();
    early_add_row_detail::queued_unsent_never_binds();
    early_add_row_detail::contradictory_reply_preserves_committed_exposure();
    early_add_row_detail::order_row_before_deal_keeps_position_risk();
}
} // namespace moex::connector_host
