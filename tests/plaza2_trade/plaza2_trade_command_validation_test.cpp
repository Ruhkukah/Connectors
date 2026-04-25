#include "plaza2_trade_test_support.hpp"

#include <iostream>

namespace {

using moex::plaza2_trade::AddOrderRequest;
using moex::plaza2_trade::IcebergAddOrderRequest;
using moex::plaza2_trade::Plaza2TradeCodec;
using moex::plaza2_trade::Plaza2TradeCommandRequest;
using moex::plaza2_trade::Plaza2TradeValidationCode;
using moex::plaza2_trade::test_support::make_add_order;
using moex::plaza2_trade::test_support::make_cod_heartbeat;
using moex::plaza2_trade::test_support::make_del_order;
using moex::plaza2_trade::test_support::make_del_orders_by_bf_limit;
using moex::plaza2_trade::test_support::make_del_user_orders;
using moex::plaza2_trade::test_support::make_iceberg_add_order;
using moex::plaza2_trade::test_support::make_iceberg_del_order;
using moex::plaza2_trade::test_support::make_iceberg_move_order;
using moex::plaza2_trade::test_support::make_move_order;
using moex::plaza2_trade::test_support::require;

void test_all_phase5a_commands_are_represented() {
    const Plaza2TradeCodec codec;
    const Plaza2TradeCommandRequest requests[] = {
        make_add_order(),          make_iceberg_add_order(), make_del_order(),
        make_iceberg_del_order(),  make_move_order(),        make_iceberg_move_order(),
        make_del_user_orders(),    make_del_orders_by_bf_limit(),
        make_cod_heartbeat(),
    };
    for (const auto& request : requests) {
        const auto result = codec.validate(request);
        require(result.ok(), "valid synthetic Phase 5A command should validate");
        const auto encoded = codec.encode(request);
        require(encoded.validation.ok(), "valid synthetic Phase 5A command should encode");
        require(!encoded.payload.empty(), "encoded command payload should not be empty");
        require(encoded.offline_only, "encoded command must be offline-only");
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
    require(codec.validate(Plaza2TradeCommandRequest{bad_quantity}).code == Plaza2TradeValidationCode::InvalidNumericRange,
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

} // namespace

int main() {
    try {
        test_all_phase5a_commands_are_represented();
        test_missing_required_field_fails();
        test_invalid_values_fail();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
