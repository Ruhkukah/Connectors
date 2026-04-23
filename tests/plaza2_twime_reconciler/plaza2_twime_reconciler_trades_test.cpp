#include "plaza2_twime_reconciler_test_support.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using moex::plaza2_twime_reconciler::make_plaza_committed_snapshot;
using moex::plaza2_twime_reconciler::MatchMode;
using moex::plaza2_twime_reconciler::normalize_twime_inbound_journal_entry;
using moex::plaza2_twime_reconciler::Plaza2TwimeReconciler;
using moex::plaza2_twime_reconciler::PlazaTradeInput;
using moex::plaza2_twime_reconciler::Side;
using moex::plaza2_twime_reconciler::TradeStatus;
using moex::plaza2_twime_reconciler::TwimeTradeInput;
using moex::plaza2_twime_reconciler::TwimeTradeInputKind;
using moex::plaza2_twime_reconciler::test_support::find_trade_by_trade_id;
using moex::plaza2_twime_reconciler::test_support::make_inbound_entry;
using moex::plaza2_twime_reconciler::test_support::make_ready_plaza_snapshot;
using moex::plaza2_twime_reconciler::test_support::price_to_mantissa;
using moex::plaza2_twime_reconciler::test_support::project_scenario;
using moex::plaza2_twime_reconciler::test_support::require;
using moex::plaza2_twime_reconciler::test_support::set_field;
using moex::twime_sbe::TwimeFieldValue;
using moex::twime_trade::test::make_request;

TwimeTradeInput make_manual_trade(std::uint64_t logical_sequence, std::int64_t order_id, std::int64_t trade_id,
                                  std::string_view price_text, std::uint32_t last_qty, std::uint32_t order_qty,
                                  Side side) {
    TwimeTradeInput input;
    input.kind = TwimeTradeInputKind::Execution;
    input.logical_sequence = logical_sequence;
    input.order_id = order_id;
    input.trade_id = trade_id;
    input.trading_session_id = 321;
    input.security_id = 1001;
    input.side = side;
    input.has_price = true;
    input.price_mantissa = price_to_mantissa(price_text);
    input.has_last_qty = true;
    input.last_qty = last_qty;
    input.has_order_qty = true;
    input.order_qty = order_qty;
    return input;
}

void test_twime_fill_matches_plaza_trade() {
    Plaza2TwimeReconciler reconciler;

    auto execution = make_request("ExecutionSingleReport");
    set_field(execution, "ClOrdID", TwimeFieldValue::unsigned_integer(501));
    set_field(execution, "OrderID", TwimeFieldValue::signed_integer(20003));
    set_field(execution, "TrdMatchID", TwimeFieldValue::signed_integer(9001));
    set_field(execution, "TradingSessionID", TwimeFieldValue::signed_integer(321));
    set_field(execution, "SecurityID", TwimeFieldValue::signed_integer(1001));
    set_field(execution, "LastPx", TwimeFieldValue::decimal(price_to_mantissa("102500")));
    set_field(execution, "LastQty", TwimeFieldValue::unsigned_integer(2));
    set_field(execution, "OrderQty", TwimeFieldValue::unsigned_integer(7));
    set_field(execution, "Side", TwimeFieldValue::enum_name("Sell"));

    const auto twime_trade = normalize_twime_inbound_journal_entry(make_inbound_entry(execution, 31), 1);
    reconciler.apply_twime_trade_input(*twime_trade.trade_input);

    const auto* twime_only = find_trade_by_trade_id(reconciler.trades(), 9001);
    require(twime_only != nullptr, "TWIME-only trade should be visible immediately");
    require(twime_only->status == TradeStatus::TwimeOnly, "trade should remain TWIME-only before PLAZA confirmation");
    require(reconciler.health().total_unmatched_twime_trades == 1, "health should count unmatched TWIME trades");

    const auto projector = project_scenario("private_state_projection");
    reconciler.apply_plaza_snapshot(make_plaza_committed_snapshot(projector, 80));

    const auto* matched = find_trade_by_trade_id(reconciler.trades(), 9001);
    require(matched != nullptr, "matched trade should remain visible");
    require(matched->status == TradeStatus::Matched, "PLAZA should confirm the TWIME fill");
    require(matched->match_mode == MatchMode::DirectIdentifier, "trade should match on direct trade id");
    require(reconciler.health().total_matched_trades == 1, "health should count matched trades");
}

void test_trade_divergence_and_ambiguity() {
    Plaza2TwimeReconciler divergence_reconciler;
    divergence_reconciler.apply_twime_trade_input(make_manual_trade(10, 32001, 91001, "102500", 2, 7, Side::Sell));

    auto divergence_snapshot = make_ready_plaza_snapshot(81);
    divergence_snapshot.trades = {
        PlazaTradeInput{
            .trade_id = 91001,
            .sess_id = 321,
            .isin_id = 1001,
            .price_text = "102500",
            .has_price = true,
            .price_mantissa = price_to_mantissa("102500"),
            .amount = 1,
            .private_order_id_sell = 32001,
        },
    };
    divergence_reconciler.apply_plaza_snapshot(divergence_snapshot);

    const auto* diverged = find_trade_by_trade_id(divergence_reconciler.trades(), 91001);
    require(diverged != nullptr, "diverged trade should remain visible");
    require(diverged->status == TradeStatus::Diverged, "mismatched direct-id trades should diverge");
    require(diverged->fault_reason == "quantity_mismatch", "trade divergence reason should be explicit");
    require(divergence_reconciler.health().total_diverged_trades == 1, "health should count diverged trades");

    Plaza2TwimeReconciler ambiguous_reconciler;
    ambiguous_reconciler.apply_twime_trade_input(make_manual_trade(11, 32002, 0, "101750", 1, 3, Side::Buy));

    auto ambiguous_snapshot = make_ready_plaza_snapshot(82);
    ambiguous_snapshot.trades = {
        PlazaTradeInput{
            .trade_id = 0,
            .sess_id = 321,
            .isin_id = 1001,
            .price_text = "101750",
            .has_price = true,
            .price_mantissa = price_to_mantissa("101750"),
            .amount = 1,
            .private_order_id_buy = 32002,
        },
        PlazaTradeInput{
            .trade_id = 0,
            .sess_id = 321,
            .isin_id = 1001,
            .price_text = "101750",
            .has_price = true,
            .price_mantissa = price_to_mantissa("101750"),
            .amount = 1,
            .private_order_id_buy = 32002,
        },
    };
    ambiguous_reconciler.apply_plaza_snapshot(ambiguous_snapshot);

    require(ambiguous_reconciler.trades().size() == 1,
            "ambiguous fallback should collapse into one explicit trade fault");
    const auto& ambiguous = ambiguous_reconciler.trades().front();
    require(ambiguous.status == TradeStatus::Ambiguous, "multiple fallback trade candidates must remain ambiguous");
    require(ambiguous.fault_reason == "multiple_plaza_trade_fallback_candidates",
            "trade ambiguity reason should explain the collision");
    require(ambiguous_reconciler.health().total_ambiguous_trades == 1, "health should count ambiguous trades");
}

} // namespace

int main() {
    try {
        test_twime_fill_matches_plaza_trade();
        test_trade_divergence_and_ambiguity();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
