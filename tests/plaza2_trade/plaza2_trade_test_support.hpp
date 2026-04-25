#pragma once

#include "moex/plaza2_trade/plaza2_trade_codec.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace moex::plaza2_trade::test_support {

inline void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline std::string fixture_text(const std::string& name) {
    const auto path = std::filesystem::path(MOEX_SOURCE_ROOT) / "tests/plaza2_trade/fixtures" / name;
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("missing fixture: " + path.string());
    }
    std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

inline AddOrderRequest make_add_order() {
    AddOrderRequest request;
    request.broker_code = "BRK1";
    request.isin_id = 123456;
    request.client_code = "C01";
    request.dir = Plaza2TradeSide::Buy;
    request.type = Plaza2TradeOrderType::Limit;
    request.amount = 10;
    request.price = "101.25";
    request.comment = "offline";
    request.ext_id = 501;
    request.is_check_limit = 1;
    return request;
}

inline IcebergAddOrderRequest make_iceberg_add_order() {
    IcebergAddOrderRequest request;
    request.broker_code = "BRK1";
    request.isin_id = 123456;
    request.client_code = "C01";
    request.dir = Plaza2TradeSide::Buy;
    request.type = Plaza2TradeOrderType::Limit;
    request.disclose_const_amount = 2;
    request.iceberg_amount = 10;
    request.price = "101.25";
    request.ext_id = 502;
    return request;
}

inline DelOrderRequest make_del_order() {
    DelOrderRequest request;
    request.broker_code = "BRK1";
    request.order_id = 9001;
    request.client_code = "C01";
    request.isin_id = 123456;
    return request;
}

inline IcebergDelOrderRequest make_iceberg_del_order() {
    IcebergDelOrderRequest request;
    request.broker_code = "BRK1";
    request.order_id = 9001;
    request.isin_id = 123456;
    return request;
}

inline MoveOrderRequest make_move_order() {
    MoveOrderRequest request;
    request.broker_code = "BRK1";
    request.regime = 0;
    request.order_id1 = 9001;
    request.amount1 = 5;
    request.price1 = "102.50";
    request.ext_id1 = 601;
    request.order_id2 = 9002;
    request.amount2 = 6;
    request.price2 = "103.00";
    request.ext_id2 = 602;
    request.is_check_limit = 1;
    request.client_code = "C01";
    request.isin_id = 123456;
    return request;
}

inline IcebergMoveOrderRequest make_iceberg_move_order() {
    IcebergMoveOrderRequest request;
    request.broker_code = "BRK1";
    request.order_id = 9001;
    request.isin_id = 123456;
    request.price = "102.50";
    request.ext_id = 701;
    return request;
}

inline DelUserOrdersRequest make_del_user_orders() {
    DelUserOrdersRequest request;
    request.broker_code = "BRK1";
    request.buy_sell = 0;
    request.non_system = 0;
    request.code = "C01";
    request.base_contract_code = "SYNTH";
    request.ext_id = 777;
    request.isin_id = 123456;
    request.instrument_mask = 1;
    return request;
}

inline DelOrdersByBFLimitRequest make_del_orders_by_bf_limit() {
    DelOrdersByBFLimitRequest request;
    request.broker_code = "BRK1";
    return request;
}

inline CODHeartbeatRequest make_cod_heartbeat() {
    CODHeartbeatRequest request;
    request.seq_number = 42;
    return request;
}

} // namespace moex::plaza2_trade::test_support
