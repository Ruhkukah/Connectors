#include "plaza2_twime_reconciler_test_support.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using moex::plaza2_twime_reconciler::make_plaza_committed_snapshot;
using moex::plaza2_twime_reconciler::OrderStatus;
using moex::plaza2_twime_reconciler::Plaza2TwimeReconciler;
using moex::plaza2_twime_reconciler::Side;
using moex::plaza2_twime_reconciler::TradeStatus;
using moex::plaza2_twime_reconciler::TwimeOrderInput;
using moex::plaza2_twime_reconciler::TwimeOrderInputKind;
using moex::plaza2_twime_reconciler::TwimeTradeInput;
using moex::plaza2_twime_reconciler::TwimeTradeInputKind;
using moex::plaza2_twime_reconciler::test_support::find_order_by_order_id;
using moex::plaza2_twime_reconciler::test_support::find_trade_by_trade_id;
using moex::plaza2_twime_reconciler::test_support::make_invalidated_plaza_snapshot;
using moex::plaza2_twime_reconciler::test_support::make_twime_health_input;
using moex::plaza2_twime_reconciler::test_support::price_to_mantissa;
using moex::plaza2_twime_reconciler::test_support::project_scenario;
using moex::plaza2_twime_reconciler::test_support::require;

TwimeOrderInput make_confirmed_order() {
    TwimeOrderInput input;
    input.kind = TwimeOrderInputKind::NewAccepted;
    input.logical_sequence = 1;
    input.cl_ord_id = 501;
    input.order_id = 20003;
    input.trading_session_id = 321;
    input.security_id = 1001;
    input.account = "CL001";
    input.side = Side::Sell;
    input.has_price = true;
    input.price_mantissa = price_to_mantissa("102500");
    input.has_order_qty = true;
    input.order_qty = 7;
    return input;
}

TwimeTradeInput make_confirmed_trade() {
    TwimeTradeInput input;
    input.kind = TwimeTradeInputKind::Execution;
    input.logical_sequence = 2;
    input.cl_ord_id = 501;
    input.order_id = 20003;
    input.trade_id = 9001;
    input.trading_session_id = 321;
    input.security_id = 1001;
    input.side = Side::Sell;
    input.has_price = true;
    input.price_mantissa = price_to_mantissa("102500");
    input.has_last_qty = true;
    input.last_qty = 2;
    input.has_order_qty = true;
    input.order_qty = 7;
    return input;
}

void test_plaza_invalidation_requires_revalidation_without_erasing_twime() {
    Plaza2TwimeReconciler reconciler;
    reconciler.update_twime_source_health(make_twime_health_input());
    reconciler.apply_twime_order_input(make_confirmed_order());
    reconciler.apply_twime_trade_input(make_confirmed_trade());

    const auto ready_projector = project_scenario("private_state_projection");
    reconciler.apply_plaza_snapshot(make_plaza_committed_snapshot(ready_projector, 90));

    const auto* confirmed_order = find_order_by_order_id(reconciler.orders(), 20003);
    const auto* matched_trade = find_trade_by_trade_id(reconciler.trades(), 9001);
    require(confirmed_order != nullptr && confirmed_order->status == OrderStatus::Confirmed,
            "ready PLAZA snapshot should confirm the order before invalidation");
    require(matched_trade != nullptr && matched_trade->status == TradeStatus::Matched,
            "ready PLAZA snapshot should match the trade before invalidation");

    const auto invalidated_projector = project_scenario("private_state_lifenum_invalidation");
    const auto invalidated_snapshot = make_plaza_committed_snapshot(invalidated_projector, 91);
    reconciler.apply_plaza_snapshot(invalidated_snapshot);

    const auto* revalidation_order = find_order_by_order_id(reconciler.orders(), 20003);
    const auto* revalidation_trade = find_trade_by_trade_id(reconciler.trades(), 9001);
    require(revalidation_order != nullptr, "TWIME-backed order should survive PLAZA invalidation");
    require(revalidation_trade != nullptr, "TWIME-backed trade should survive PLAZA invalidation");
    require(revalidation_order->status == OrderStatus::Confirmed,
            "PLAZA invalidation should not silently destroy previously confirmed order state");
    require(revalidation_order->plaza_revalidation_required,
            "confirmed order should be marked for PLAZA revalidation after invalidation");
    require(revalidation_trade->status == TradeStatus::Matched,
            "PLAZA invalidation should not silently destroy previously matched trade state");
    require(revalidation_trade->plaza_revalidation_required,
            "matched trade should be marked for PLAZA revalidation after invalidation");

    const auto& health = reconciler.health();
    require(health.plaza.invalidated, "health should reflect PLAZA invalidation");
    require(health.plaza.last_invalidation_reason == "connector_not_online",
            "health should retain the invalidation reason");
    require(health.plaza_revalidation_pending_orders == 1,
            "health should count order entries awaiting PLAZA revalidation");
    require(health.plaza_revalidation_pending_trades == 1,
            "health should count trade entries awaiting PLAZA revalidation");
}

void test_plaza_only_counts_remain_explicit() {
    Plaza2TwimeReconciler reconciler;
    auto invalidated_only = make_invalidated_plaza_snapshot("private_stream_not_ready", 12);
    reconciler.apply_plaza_snapshot(invalidated_only);

    require(reconciler.orders().empty(), "invalidated snapshot without prior PLAZA state should not invent orders");
    require(reconciler.trades().empty(), "invalidated snapshot without prior PLAZA state should not invent trades");
    require(reconciler.health().plaza.invalidated, "health should retain invalidated-only state");
    require(reconciler.health().plaza.last_lifenum == 12, "health should surface latest invalidated lifenum");
}

} // namespace

int main() {
    try {
        test_plaza_invalidation_requires_revalidation_without_erasing_twime();
        test_plaza_only_counts_remain_explicit();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
