#include "plaza2_twime_reconciler_test_support.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using moex::plaza2_twime_reconciler::collect_twime_reconciliation_inputs;
using moex::plaza2_twime_reconciler::make_plaza_committed_snapshot;
using moex::plaza2_twime_reconciler::MatchMode;
using moex::plaza2_twime_reconciler::normalize_twime_inbound_journal_entry;
using moex::plaza2_twime_reconciler::normalize_twime_outbound_request;
using moex::plaza2_twime_reconciler::PlazaCommittedSnapshotInput;
using moex::plaza2_twime_reconciler::Side;
using moex::plaza2_twime_reconciler::TwimeOrderInputKind;
using moex::plaza2_twime_reconciler::TwimeTradeInputKind;
using moex::plaza2_twime_reconciler::test_support::make_inbound_entry;
using moex::plaza2_twime_reconciler::test_support::make_twime_health_input;
using moex::plaza2_twime_reconciler::test_support::price_to_mantissa;
using moex::plaza2_twime_reconciler::test_support::project_scenario;
using moex::plaza2_twime_reconciler::test_support::require;
using moex::plaza2_twime_reconciler::test_support::set_field;
using moex::twime_sbe::TwimeFieldValue;
using moex::twime_trade::test::make_request;

void test_outbound_request_normalization() {
    auto new_order = make_request("NewOrderSingle");
    set_field(new_order, "ClOrdID", TwimeFieldValue::unsigned_integer(501));
    set_field(new_order, "Price", TwimeFieldValue::decimal(price_to_mantissa("102500")));
    set_field(new_order, "SecurityID", TwimeFieldValue::signed_integer(1001));
    set_field(new_order, "OrderQty", TwimeFieldValue::unsigned_integer(7));
    set_field(new_order, "Side", TwimeFieldValue::enum_name("Sell"));
    set_field(new_order, "Account", TwimeFieldValue::string("CL001"));

    const auto new_result = normalize_twime_outbound_request(new_order, 10);
    require(new_result.ok, "new order normalization should succeed");
    require(new_result.order_input.has_value(), "new order should produce a normalized order input");
    require(new_result.order_input->kind == TwimeOrderInputKind::NewIntent, "new order should stay provisional");
    require(new_result.order_input->cl_ord_id == 501, "ClOrdID should be normalized");
    require(new_result.order_input->account == "CL001", "account should be normalized");
    require(new_result.order_input->side == Side::Sell, "side should be normalized");
    require(new_result.order_input->price_mantissa == price_to_mantissa("102500"),
            "price mantissa should be preserved");

    auto replace_order = make_request("OrderReplaceRequest");
    set_field(replace_order, "ClOrdID", TwimeFieldValue::unsigned_integer(777));
    set_field(replace_order, "OrderID", TwimeFieldValue::signed_integer(40001));
    set_field(replace_order, "Price", TwimeFieldValue::decimal(price_to_mantissa("103000")));
    set_field(replace_order, "OrderQty", TwimeFieldValue::unsigned_integer(9));
    set_field(replace_order, "SecurityID", TwimeFieldValue::signed_integer(1001));
    set_field(replace_order, "Account", TwimeFieldValue::string("CL001"));

    const auto replace_result = normalize_twime_outbound_request(replace_order, 11);
    require(replace_result.ok, "replace normalization should succeed");
    require(replace_result.order_input.has_value(), "replace should produce a normalized order input");
    require(replace_result.order_input->kind == TwimeOrderInputKind::ReplaceIntent,
            "replace request should remain provisional");
    require(replace_result.order_input->prev_order_id == 40001, "replace request should retain target order id");
    require(replace_result.order_input->order_id == 0, "replace request should not pretend the new order id is known");

    auto cancel_order = make_request("OrderCancelRequest");
    set_field(cancel_order, "ClOrdID", TwimeFieldValue::unsigned_integer(778));
    set_field(cancel_order, "OrderID", TwimeFieldValue::signed_integer(40001));
    set_field(cancel_order, "SecurityID", TwimeFieldValue::signed_integer(1001));
    set_field(cancel_order, "Account", TwimeFieldValue::string("CL001"));

    const auto cancel_result = normalize_twime_outbound_request(cancel_order, 12);
    require(cancel_result.ok, "cancel normalization should succeed");
    require(cancel_result.order_input.has_value(), "cancel should produce a normalized order input");
    require(cancel_result.order_input->kind == TwimeOrderInputKind::CancelIntent,
            "cancel request should remain provisional");
    require(cancel_result.order_input->order_id == 40001, "cancel request should retain target order id");
}

void test_inbound_journal_normalization_and_batch_collection() {
    auto accepted = make_request("NewOrderSingleResponse");
    set_field(accepted, "ClOrdID", TwimeFieldValue::unsigned_integer(501));
    set_field(accepted, "OrderID", TwimeFieldValue::signed_integer(20003));
    set_field(accepted, "TradingSessionID", TwimeFieldValue::signed_integer(321));
    set_field(accepted, "SecurityID", TwimeFieldValue::signed_integer(1001));
    set_field(accepted, "OrderQty", TwimeFieldValue::unsigned_integer(7));
    set_field(accepted, "Price", TwimeFieldValue::decimal(price_to_mantissa("102500")));
    set_field(accepted, "Side", TwimeFieldValue::enum_name("Sell"));

    const auto accepted_result = normalize_twime_inbound_journal_entry(make_inbound_entry(accepted, 21), 30);
    require(accepted_result.ok, "accepted response normalization should succeed");
    require(accepted_result.order_input.has_value(), "accepted response should normalize to an order");
    require(accepted_result.order_input->kind == TwimeOrderInputKind::NewAccepted,
            "accepted response should be marked as accepted");
    require(accepted_result.order_input->order_id == 20003, "accepted response should normalize order id");

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

    const auto execution_result = normalize_twime_inbound_journal_entry(make_inbound_entry(execution, 22), 31);
    require(execution_result.ok, "execution report normalization should succeed");
    require(execution_result.trade_input.has_value(), "execution report should normalize to a trade");
    require(execution_result.trade_input->kind == TwimeTradeInputKind::Execution,
            "execution report should preserve its kind");
    require(execution_result.trade_input->trade_id == 9001, "execution report should normalize trade id");

    auto reject = make_request("BusinessMessageReject");
    set_field(reject, "ClOrdID", TwimeFieldValue::unsigned_integer(601));
    set_field(reject, "OrdRejReason", TwimeFieldValue::signed_integer(-42));

    const auto reject_result = normalize_twime_inbound_journal_entry(make_inbound_entry(reject, 23), 32);
    require(reject_result.ok, "business reject normalization should succeed");
    require(reject_result.order_input.has_value(), "business reject should normalize to an order");
    require(reject_result.order_input->kind == TwimeOrderInputKind::Rejected,
            "business reject should surface a rejected order input");
    require(reject_result.order_input->reject_code == -42, "business reject should surface the reject code");

    auto outbound = make_request("NewOrderSingle");
    set_field(outbound, "ClOrdID", TwimeFieldValue::unsigned_integer(700));
    set_field(outbound, "Price", TwimeFieldValue::decimal(price_to_mantissa("102400")));
    set_field(outbound, "SecurityID", TwimeFieldValue::signed_integer(1001));
    set_field(outbound, "OrderQty", TwimeFieldValue::unsigned_integer(3));
    set_field(outbound, "Side", TwimeFieldValue::enum_name("Buy"));
    set_field(outbound, "Account", TwimeFieldValue::string("CL001"));

    const auto source_health = make_twime_health_input();
    std::vector<moex::twime_sbe::TwimeEncodeRequest> outbound_requests{outbound};
    std::vector<moex::twime_trade::TwimeJournalEntry> inbound_entries{
        make_inbound_entry(accepted, 24),
        make_inbound_entry(execution, 25),
    };

    const auto batch = collect_twime_reconciliation_inputs(
        outbound_requests, inbound_entries, &source_health.session_health, &source_health.session_metrics, 50);
    require(batch.ok, "batch collection should succeed");
    require(batch.source_health.has_value(), "source health should be carried into the batch");
    require(batch.order_inputs.size() == 2, "batch should contain outbound intent and inbound acceptance");
    require(batch.trade_inputs.size() == 1, "batch should contain the execution report");
}

void test_plaza_snapshot_normalization() {
    const auto projector = project_scenario("private_state_projection");
    const auto snapshot = make_plaza_committed_snapshot(projector, 55);
    require(snapshot.source_health.required_private_streams_ready, "Phase 3E snapshot should look ready");
    require(!snapshot.source_health.invalidated, "ready Phase 3E snapshot should not be marked invalid");
    require(snapshot.orders.size() == 2, "Phase 3E snapshot should export committed own orders");
    require(snapshot.trades.size() == 1, "Phase 3E snapshot should export committed own trades");

    const auto merged_order = std::find_if(snapshot.orders.begin(), snapshot.orders.end(),
                                           [](const auto& order) { return order.private_order_id == 20003; });
    require(merged_order != snapshot.orders.end(), "snapshot should include the merged own order");
    require(merged_order->price_mantissa == price_to_mantissa("102500"),
            "PLAZA order price should normalize to decimal-5 mantissa");

    const auto lifenum_projector = project_scenario("private_state_lifenum_invalidation");
    const auto invalidated = make_plaza_committed_snapshot(lifenum_projector, 56);
    require(invalidated.source_health.invalidated, "lifenum invalidation should surface through the snapshot adapter");
    require(!invalidated.source_health.required_private_streams_ready,
            "invalidated snapshot should require revalidation");
    require(invalidated.source_health.invalidation_reason == "connector_not_online",
            "invalidated snapshot should explain the connector-side failure");
}

} // namespace

int main() {
    try {
        test_outbound_request_normalization();
        test_inbound_journal_normalization_and_batch_collection();
        test_plaza_snapshot_normalization();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
