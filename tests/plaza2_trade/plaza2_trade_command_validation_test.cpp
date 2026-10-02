#include "plaza2_trade_test_support.hpp"

#include <iostream>

namespace {

using moex::plaza2_trade::AddOrderRequest;
using moex::plaza2_trade::Plaza2TradeCodec;
using moex::plaza2_trade::Plaza2TradeCommandRequest;
using moex::plaza2_trade::Plaza2TradeValidationCode;
using moex::plaza2_trade::test_support::make_add_order;
using moex::plaza2_trade::test_support::make_del_order;
using moex::plaza2_trade::test_support::make_del_user_orders;
using moex::plaza2_trade::test_support::make_move_order;
using moex::plaza2_trade::test_support::require;

void test_declared_commands_validate() {
    const Plaza2TradeCodec codec;
    const Plaza2TradeCommandRequest requests[] = {make_add_order(), make_del_order(), make_move_order(),
                                                  make_del_user_orders()};
    for (const auto& request : requests) {
        const auto result = codec.validate(request);
        require(result.ok(), "valid declared command should validate");
        const auto encoded = codec.encode(request);
        require(encoded.validation.ok(), "valid declared command should encode");
        require(!encoded.payload.empty(), "encoded command payload should not be empty");
    }
}

void test_missing_required_field_fails() {
    const Plaza2TradeCodec codec;
    auto request = make_add_order();
    request.client_code.reset();

    const auto result = codec.validate(Plaza2TradeCommandRequest{request});
    require(result.code == Plaza2TradeValidationCode::MissingRequiredField, "missing required field should fail");
    require(result.field_name == "client_code", "missing field name should be explicit");
}

void test_invalid_values_fail() {
    const Plaza2TradeCodec codec;

    auto bad_side = make_add_order();
    bad_side.dir = static_cast<moex::plaza2_trade::Plaza2TradeSide>(99);
    require(codec.validate(Plaza2TradeCommandRequest{bad_side}).code == Plaza2TradeValidationCode::InvalidEnum,
            "invalid side should fail");

    auto bad_quantity = make_add_order();
    bad_quantity.amount = 0;
    require(codec.validate(Plaza2TradeCommandRequest{bad_quantity}).code ==
                Plaza2TradeValidationCode::InvalidNumericRange,
            "zero order quantity should fail");

    auto bad_price = make_add_order();
    bad_price.price = "101,25";
    require(codec.validate(Plaza2TradeCommandRequest{bad_price}).code == Plaza2TradeValidationCode::InvalidDecimalText,
            "locale-dependent price text should fail");

    auto overlong = make_add_order();
    overlong.broker_code = "BROKER";
    require(codec.validate(Plaza2TradeCommandRequest{overlong}).code == Plaza2TradeValidationCode::StringTooLong,
            "overlong fixed-width string should fail");
}

void test_exchange_command_enums() {
    const Plaza2TradeCodec codec;
    for (const int value : {-1, 0, 4}) {
        auto request = make_del_user_orders();
        request.buy_sell = value;
        require(codec.validate(request).code == Plaza2TradeValidationCode::InvalidEnum,
                "DelUserOrders direction must be 1, 2 or 3");
    }
    for (const int value : {1, 2, 3}) {
        auto request = make_del_user_orders();
        request.buy_sell = value;
        const auto encoded = codec.encode(request);
        require(encoded.validation.ok() && encoded.isin_id == 123456, "valid DelUserOrders direction rejected");
        require(encoded.fields_json.find("\"buy_sell\":" + std::to_string(value)) != std::string::npos,
                "decoded command log omits direction");
    }
    auto move = make_move_order();
    move.regime = 4;
    require(codec.validate(move).code == Plaza2TradeValidationCode::InvalidEnum, "undefined Move regime accepted");
    move.regime = 3;
    require(codec.encode(move).validation.ok(), "matched-quantity preserving Move regime rejected");
}

} // namespace

int main() {
    try {
        test_declared_commands_validate();
        test_missing_required_field_fails();
        test_invalid_values_fail();
        test_exchange_command_enums();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
