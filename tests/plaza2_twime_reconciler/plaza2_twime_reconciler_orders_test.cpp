#include "plaza2_twime_reconciler_test_support.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using moex::plaza2_twime_reconciler::make_plaza_committed_snapshot;
using moex::plaza2_twime_reconciler::MatchMode;
using moex::plaza2_twime_reconciler::normalize_twime_inbound_journal_entry;
using moex::plaza2_twime_reconciler::normalize_twime_outbound_request;
using moex::plaza2_twime_reconciler::OrderStatus;
using moex::plaza2_twime_reconciler::Plaza2TwimeReconciler;
using moex::plaza2_twime_reconciler::PlazaCommittedSnapshotInput;
using moex::plaza2_twime_reconciler::PlazaOrderInput;
using moex::plaza2_twime_reconciler::Side;
using moex::plaza2_twime_reconciler::TwimeOrderInput;
using moex::plaza2_twime_reconciler::TwimeOrderInputKind;
using moex::plaza2_twime_reconciler::TwimeTradeInput;
using moex::plaza2_twime_reconciler::TwimeTradeInputKind;
using moex::plaza2_twime_reconciler::test_support::find_order_by_cl_ord_id;
using moex::plaza2_twime_reconciler::test_support::find_order_by_order_id;
using moex::plaza2_twime_reconciler::test_support::make_inbound_entry;
using moex::plaza2_twime_reconciler::test_support::make_ready_plaza_snapshot;
using moex::plaza2_twime_reconciler::test_support::make_twime_health_input;
using moex::plaza2_twime_reconciler::test_support::price_to_mantissa;
using moex::plaza2_twime_reconciler::test_support::project_scenario;
using moex::plaza2_twime_reconciler::test_support::require;
using moex::plaza2_twime_reconciler::test_support::set_field;
using moex::twime_sbe::TwimeFieldValue;
using moex::twime_trade::test::make_request;

TwimeOrderInput make_manual_order(std::uint64_t logical_sequence, std::uint64_t cl_ord_id, std::int64_t order_id,
                                  TwimeOrderInputKind kind, std::string account, std::string_view price_text,
                                  std::uint32_t order_qty, Side side) {
    TwimeOrderInput input;
    input.kind = kind;
    input.logical_sequence = logical_sequence;
    input.cl_ord_id = cl_ord_id;
    input.order_id = order_id;
    input.trading_session_id = 321;
    input.security_id = 1001;
    input.account = std::move(account);
    input.side = side;
    input.has_price = true;
    input.price_mantissa = price_to_mantissa(price_text);
    input.has_order_qty = true;
    input.order_qty = order_qty;
    return input;
}

TwimeTradeInput make_manual_fill(std::uint64_t logical_sequence, std::int64_t order_id, std::int64_t trade_id,
                                 std::uint32_t last_qty, std::uint32_t order_qty, Side side) {
    TwimeTradeInput input;
    input.kind = TwimeTradeInputKind::Execution;
    input.logical_sequence = logical_sequence;
    input.order_id = order_id;
    input.trade_id = trade_id;
    input.trading_session_id = 321;
    input.security_id = 1001;
    input.side = side;
    input.has_price = true;
    input.price_mantissa = price_to_mantissa("102500");
    input.has_last_qty = true;
    input.last_qty = last_qty;
    input.has_order_qty = true;
    input.order_qty = order_qty;
    return input;
}

void test_provisional_confirmed_path() {
    Plaza2TwimeReconciler reconciler(4);
    reconciler.update_twime_source_health(make_twime_health_input());

    auto new_order = make_request("NewOrderSingle");
    set_field(new_order, "ClOrdID", TwimeFieldValue::unsigned_integer(501));
    set_field(new_order, "Price", TwimeFieldValue::decimal(price_to_mantissa("102500")));
    set_field(new_order, "SecurityID", TwimeFieldValue::signed_integer(1001));
    set_field(new_order, "OrderQty", TwimeFieldValue::unsigned_integer(7));
    set_field(new_order, "Side", TwimeFieldValue::enum_name("Sell"));
    set_field(new_order, "Account", TwimeFieldValue::string("CL001"));

    const auto provisional = normalize_twime_outbound_request(new_order, 1);
    reconciler.apply_twime_order_input(*provisional.order_input);

    const auto* provisional_order = find_order_by_cl_ord_id(reconciler.orders(), 501);
    require(provisional_order != nullptr, "provisional TWIME order should be visible immediately");
    require(provisional_order->status == OrderStatus::ProvisionalTwime,
            "outbound TWIME intent should remain provisional before confirmation");
    require(reconciler.health().total_provisional_orders == 1, "health should count provisional TWIME orders");
    require(reconciler.health().total_unmatched_twime_orders == 1, "health should count unmatched TWIME orders");

    auto accepted = make_request("NewOrderSingleResponse");
    set_field(accepted, "ClOrdID", TwimeFieldValue::unsigned_integer(501));
    set_field(accepted, "OrderID", TwimeFieldValue::signed_integer(20003));
    set_field(accepted, "TradingSessionID", TwimeFieldValue::signed_integer(321));
    set_field(accepted, "SecurityID", TwimeFieldValue::signed_integer(1001));
    set_field(accepted, "OrderQty", TwimeFieldValue::unsigned_integer(7));
    set_field(accepted, "Price", TwimeFieldValue::decimal(price_to_mantissa("102500")));
    set_field(accepted, "Side", TwimeFieldValue::enum_name("Sell"));

    const auto accepted_result = normalize_twime_inbound_journal_entry(make_inbound_entry(accepted, 21), 2);
    reconciler.apply_twime_order_input(*accepted_result.order_input);

    const auto* accepted_order = find_order_by_order_id(reconciler.orders(), 20003);
    require(accepted_order != nullptr, "accepted order should be addressable by exchange order id");
    require(accepted_order->status == OrderStatus::ProvisionalTwime,
            "TWIME acceptance should stay provisional until PLAZA confirms it");

    const auto projector = project_scenario("private_state_projection");
    reconciler.apply_plaza_snapshot(make_plaza_committed_snapshot(projector, 50));

    const auto* confirmed_order = find_order_by_order_id(reconciler.orders(), 20003);
    require(confirmed_order != nullptr, "PLAZA-confirmed order should remain visible");
    require(confirmed_order->status == OrderStatus::Confirmed, "PLAZA should promote the order to confirmed");
    require(confirmed_order->match_mode == MatchMode::DirectIdentifier,
            "confirmed order should match on direct exchange order id");
    require(confirmed_order->last_update_source == moex::plaza2_twime_reconciler::ReconciliationSource::Plaza,
            "PLAZA confirmation should become the last update source");
    require(reconciler.health().total_confirmed_orders == 1, "health should count confirmed orders");
    require(reconciler.health().twime.present, "reconciler health should surface TWIME source status");
    require(reconciler.health().plaza.present, "reconciler health should surface PLAZA source status");
}

void test_rejected_stale_canceled_and_filled_paths() {
    Plaza2TwimeReconciler stale_reconciler(2);
    stale_reconciler.apply_twime_order_input(
        make_manual_order(1, 601, 0, TwimeOrderInputKind::NewIntent, "CL001", "101000", 4, Side::Buy));
    stale_reconciler.advance_steps(2);

    const auto* stale_order = find_order_by_cl_ord_id(stale_reconciler.orders(), 601);
    require(stale_order != nullptr, "stale provisional order should remain queryable");
    require(stale_order->status == OrderStatus::Stale, "logical-step advancement should mark stale orders");
    require(stale_reconciler.health().total_stale_provisional_orders == 1,
            "health should count stale provisional orders");

    Plaza2TwimeReconciler rejected_reconciler;
    auto rejected = make_manual_order(2, 602, 0, TwimeOrderInputKind::Rejected, "CL001", "101500", 2, Side::Sell);
    rejected.reject_code = -17;
    rejected_reconciler.apply_twime_order_input(rejected);

    const auto* rejected_order = find_order_by_cl_ord_id(rejected_reconciler.orders(), 602);
    require(rejected_order != nullptr, "rejected order should remain queryable");
    require(rejected_order->status == OrderStatus::Rejected, "TWIME reject should remain explicit");
    require(rejected_order->twime.reject_code == -17, "reject code should be preserved");

    Plaza2TwimeReconciler terminal_reconciler;
    terminal_reconciler.apply_twime_order_input(
        make_manual_order(3, 603, 41001, TwimeOrderInputKind::NewAccepted, "CL001", "102500", 2, Side::Sell));
    terminal_reconciler.apply_twime_trade_input(make_manual_fill(4, 41001, 91001, 2, 2, Side::Sell));

    const auto* filled_order = find_order_by_order_id(terminal_reconciler.orders(), 41001);
    require(filled_order != nullptr, "filled order should remain queryable");
    require(filled_order->status == OrderStatus::Filled, "TWIME fill should promote the order to filled");

    Plaza2TwimeReconciler cancel_reconciler;
    cancel_reconciler.apply_twime_order_input(
        make_manual_order(5, 604, 42001, TwimeOrderInputKind::NewAccepted, "CL001", "102000", 3, Side::Buy));
    cancel_reconciler.apply_twime_order_input(
        make_manual_order(6, 605, 42001, TwimeOrderInputKind::CancelAccepted, "CL001", "102000", 3, Side::Buy));

    const auto* canceled_order = find_order_by_order_id(cancel_reconciler.orders(), 42001);
    require(canceled_order != nullptr, "canceled order should remain queryable");
    require(canceled_order->status == OrderStatus::Canceled, "TWIME cancel acceptance should remain explicit");
}

void test_diverged_and_ambiguous_orders() {
    Plaza2TwimeReconciler divergence_reconciler;
    divergence_reconciler.apply_twime_order_input(
        make_manual_order(10, 701, 20003, TwimeOrderInputKind::NewAccepted, "CL001", "102600", 7, Side::Sell));

    const auto projector = project_scenario("private_state_projection");
    divergence_reconciler.apply_plaza_snapshot(make_plaza_committed_snapshot(projector, 70));

    const auto* diverged = find_order_by_order_id(divergence_reconciler.orders(), 20003);
    require(diverged != nullptr, "diverged order should remain queryable");
    require(diverged->status == OrderStatus::Diverged, "mismatched direct-id orders should diverge");
    require(diverged->fault_reason == "price_mismatch", "divergence reason should be explicit");
    require(divergence_reconciler.health().total_diverged_orders == 1, "health should count diverged orders");

    Plaza2TwimeReconciler ambiguous_reconciler;
    ambiguous_reconciler.apply_twime_order_input(
        make_manual_order(11, 702, 0, TwimeOrderInputKind::NewIntent, "CL001", "101250", 3, Side::Buy));

    auto ambiguous_snapshot = make_ready_plaza_snapshot(71);
    ambiguous_snapshot.orders = {
        PlazaOrderInput{
            .public_order_id = 1,
            .private_order_id = 51001,
            .sess_id = 321,
            .isin_id = 1001,
            .client_code = "CL001",
            .price_text = "101250",
            .has_price = true,
            .price_mantissa = price_to_mantissa("101250"),
            .public_amount = 3,
            .private_amount = 3,
            .side = Side::Buy,
        },
        PlazaOrderInput{
            .public_order_id = 2,
            .private_order_id = 51002,
            .sess_id = 321,
            .isin_id = 1001,
            .client_code = "CL001",
            .price_text = "101250",
            .has_price = true,
            .price_mantissa = price_to_mantissa("101250"),
            .public_amount = 3,
            .private_amount = 3,
            .side = Side::Buy,
        },
    };
    ambiguous_reconciler.apply_plaza_snapshot(ambiguous_snapshot);

    const auto* ambiguous = find_order_by_cl_ord_id(ambiguous_reconciler.orders(), 702);
    require(ambiguous != nullptr, "ambiguous order should remain queryable");
    require(ambiguous->status == OrderStatus::Ambiguous, "multiple fallback candidates must not be guessed");
    require(ambiguous->fault_reason == "multiple_plaza_fallback_candidates",
            "ambiguity reason should explain the fallback collision");
    require(ambiguous_reconciler.health().total_ambiguous_orders == 1, "health should count ambiguous orders");
}

} // namespace

int main() {
    try {
        test_provisional_confirmed_path();
        test_rejected_stale_canceled_and_filled_paths();
        test_diverged_and_ambiguous_orders();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
