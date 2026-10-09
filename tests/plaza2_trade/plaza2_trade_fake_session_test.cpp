#include "moex/plaza2_trade/plaza2_trade_fake_session.hpp"

#include "plaza2_trade_test_support.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using moex::plaza2_trade::DelOrderRequest;
using moex::plaza2_trade::Plaza2TradeCommandRequest;
using moex::plaza2_trade::Plaza2TradeFakeOrderStatus;
using moex::plaza2_trade::Plaza2TradeFakeOutcomeStatus;
using moex::plaza2_trade::Plaza2TradeFakeSession;
using moex::plaza2_trade::test_support::make_add_order;
using moex::plaza2_trade::test_support::make_del_order;
using moex::plaza2_trade::test_support::make_del_user_orders;
using moex::plaza2_trade::test_support::make_move_order;
using moex::plaza2_trade::test_support::require;

void test_rejects_before_established() {
    Plaza2TradeFakeSession session;
    const auto result = session.submit(Plaza2TradeCommandRequest{make_add_order()});
    require(result.status == Plaza2TradeFakeOutcomeStatus::InvalidState,
            "fake session should reject before established");
    require(session.orders().empty(), "invalid-state command should not mutate order state");
}

void test_add_duplicate_and_unknown_cancel() {
    Plaza2TradeFakeSession session;
    session.establish();

    const auto accepted = session.submit(Plaza2TradeCommandRequest{make_add_order()});
    require(accepted.status == Plaza2TradeFakeOutcomeStatus::Accepted, "AddOrder should be accepted");
    require(accepted.reply_msgid == 179, "AddOrder must use its declared exchange reply");
    require(accepted.generated_order_id.has_value(), "accepted AddOrder should generate synthetic order id");
    require(!accepted.replication.empty(), "accepted AddOrder should emit fake replication");
    require(session.orders().size() == 1, "accepted AddOrder should create one fake order");
    require(session.orders()[0].status == Plaza2TradeFakeOrderStatus::Active, "new fake order should be active");

    const auto duplicate = session.submit(Plaza2TradeCommandRequest{make_add_order()});
    require(duplicate.status == Plaza2TradeFakeOutcomeStatus::DuplicateClientTransactionId,
            "duplicate client transaction id should reject deterministically");
    require(session.orders().size() == 1, "duplicate command should not create another order");

    auto unknown = make_del_order();
    unknown.order_id = 999999;
    const auto unknown_result = session.submit(Plaza2TradeCommandRequest{unknown});
    require(unknown_result.status == Plaza2TradeFakeOutcomeStatus::UnknownOrder, "unknown order cancel should reject");
}

void test_cancel_move_and_mass_cancel() {
    Plaza2TradeFakeSession session;
    session.establish();

    auto first = make_add_order();
    first.ext_id = 501;
    const auto first_result = session.submit(Plaza2TradeCommandRequest{first});
    auto second = make_add_order();
    second.ext_id = 502;
    const auto second_result = session.submit(Plaza2TradeCommandRequest{second});
    require(first_result.status == Plaza2TradeFakeOutcomeStatus::Accepted, "first add should accept");
    require(second_result.status == Plaza2TradeFakeOutcomeStatus::Accepted, "second add should accept");

    auto move = make_move_order();
    move.order_id1 = *first_result.generated_order_id;
    move.ext_id1 = 601;
    move.price1 = "105.75";
    move.amount1 = 3;
    const auto moved = session.submit(Plaza2TradeCommandRequest{move});
    require(moved.status == Plaza2TradeFakeOutcomeStatus::Accepted, "MoveOrder should accept for active order");
    require(moved.reply_msgid == 176, "MoveOrder must use its declared exchange reply");
    require(session.orders()[0].status == Plaza2TradeFakeOrderStatus::Moved, "fake order should record move status");
    require(session.orders()[0].price == "105.75", "fake move should update price");

    auto cancel = make_del_order();
    cancel.order_id = *first_result.generated_order_id;
    const auto canceled = session.submit(Plaza2TradeCommandRequest{cancel});
    require(canceled.status == Plaza2TradeFakeOutcomeStatus::Accepted, "DelOrder should cancel active order");
    require(canceled.reply_msgid == 177, "DelOrder must use its declared exchange reply");
    require(session.orders()[0].status == Plaza2TradeFakeOrderStatus::Canceled, "fake order should become canceled");

    auto mass = make_del_user_orders();
    mass.ext_id = 777;
    const auto mass_result = session.submit(Plaza2TradeCommandRequest{mass});
    require(mass_result.status == Plaza2TradeFakeOutcomeStatus::Accepted, "DelUserOrders should accept");
    require(mass_result.reply_msgid == 186, "DelUserOrders must use its declared exchange reply");
    require(session.orders()[1].status == Plaza2TradeFakeOrderStatus::Canceled, "matching order should be canceled");
    require(mass_result.decoded_reply.num_orders && *mass_result.decoded_reply.num_orders == 1,
            "mass cancel count should be deterministic");
}

} // namespace

int main() {
    try {
        test_rejects_before_established();
        test_add_duplicate_and_unknown_cancel();
        test_cancel_move_and_mass_cancel();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
