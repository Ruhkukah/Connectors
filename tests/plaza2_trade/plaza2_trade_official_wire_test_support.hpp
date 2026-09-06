#pragma once
#include "fixtures/cgate99_messages.hpp"
#include "plaza2_trade_test_support.hpp"
#include <cstring>
#include <type_traits>
namespace moex::plaza2_trade::test_support {
template <typename T, typename U> T wire_scalar(const std::optional<U>& value) {
    return value ? static_cast<T>(*value) : T{};
}
template <std::size_t N> void assign_wire(char (&field)[N], const std::optional<std::string>& value) {
    if (value)
        std::memcpy(field, value->data(), value->size());
}
template <typename T> std::vector<std::byte> wire_bytes(const T& wire) {
    const auto* first = reinterpret_cast<const std::byte*>(&wire);
    return {first, first + sizeof(wire)};
}

inline std::vector<std::byte> official_wire(const AddOrderRequest& request) {
    official_cgate99::AddOrder wire;
    std::memset(&wire, 0, sizeof(wire));
    static_assert(sizeof(wire) == official_cgate99::sizeof_AddOrder);
    assign_wire(wire.broker_code, request.broker_code);
    wire.isin_id = wire_scalar<decltype(wire.isin_id)>(request.isin_id);
    assign_wire(wire.client_code, request.client_code);
    wire.dir = wire_scalar<decltype(wire.dir)>(request.dir);
    wire.type = wire_scalar<decltype(wire.type)>(request.type);
    wire.amount = wire_scalar<decltype(wire.amount)>(request.amount);
    assign_wire(wire.price, request.price);
    assign_wire(wire.comment, request.comment);
    assign_wire(wire.broker_to, request.broker_to);
    wire.ext_id = wire_scalar<decltype(wire.ext_id)>(request.ext_id);
    wire.is_check_limit = wire_scalar<decltype(wire.is_check_limit)>(request.is_check_limit);
    assign_wire(wire.date_exp, request.date_exp);
    wire.dont_check_money = wire_scalar<decltype(wire.dont_check_money)>(request.dont_check_money);
    assign_wire(wire.match_ref, request.match_ref);
    wire.ncc_request = wire_scalar<decltype(wire.ncc_request)>(request.ncc_request);
    assign_wire(wire.compliance_id, request.compliance_id);
    return wire_bytes(wire);
}

inline std::vector<std::byte> official_wire(const IcebergAddOrderRequest& request) {
    official_cgate99::IcebergAddOrder wire;
    std::memset(&wire, 0, sizeof(wire));
    static_assert(sizeof(wire) == official_cgate99::sizeof_IcebergAddOrder);
    assign_wire(wire.broker_code, request.broker_code);
    wire.isin_id = wire_scalar<decltype(wire.isin_id)>(request.isin_id);
    assign_wire(wire.client_code, request.client_code);
    wire.dir = wire_scalar<decltype(wire.dir)>(request.dir);
    wire.type = wire_scalar<decltype(wire.type)>(request.type);
    wire.disclose_const_amount = wire_scalar<decltype(wire.disclose_const_amount)>(request.disclose_const_amount);
    wire.iceberg_amount = wire_scalar<decltype(wire.iceberg_amount)>(request.iceberg_amount);
    wire.variance_amount = wire_scalar<decltype(wire.variance_amount)>(request.variance_amount);
    assign_wire(wire.price, request.price);
    assign_wire(wire.comment, request.comment);
    wire.ext_id = wire_scalar<decltype(wire.ext_id)>(request.ext_id);
    wire.is_check_limit = wire_scalar<decltype(wire.is_check_limit)>(request.is_check_limit);
    assign_wire(wire.date_exp, request.date_exp);
    wire.dont_check_money = wire_scalar<decltype(wire.dont_check_money)>(request.dont_check_money);
    wire.ncc_request = wire_scalar<decltype(wire.ncc_request)>(request.ncc_request);
    assign_wire(wire.compliance_id, request.compliance_id);
    return wire_bytes(wire);
}

inline std::vector<std::byte> official_wire(const DelOrderRequest& request) {
    official_cgate99::DelOrder wire;
    std::memset(&wire, 0, sizeof(wire));
    static_assert(sizeof(wire) == official_cgate99::sizeof_DelOrder);
    assign_wire(wire.broker_code, request.broker_code);
    wire.order_id = wire_scalar<decltype(wire.order_id)>(request.order_id);
    wire.ncc_request = wire_scalar<decltype(wire.ncc_request)>(request.ncc_request);
    assign_wire(wire.client_code, request.client_code);
    wire.isin_id = wire_scalar<decltype(wire.isin_id)>(request.isin_id);
    return wire_bytes(wire);
}

inline std::vector<std::byte> official_wire(const IcebergDelOrderRequest& request) {
    official_cgate99::IcebergDelOrder wire;
    std::memset(&wire, 0, sizeof(wire));
    static_assert(sizeof(wire) == official_cgate99::sizeof_IcebergDelOrder);
    assign_wire(wire.broker_code, request.broker_code);
    wire.order_id = wire_scalar<decltype(wire.order_id)>(request.order_id);
    wire.isin_id = wire_scalar<decltype(wire.isin_id)>(request.isin_id);
    wire.ncc_request = wire_scalar<decltype(wire.ncc_request)>(request.ncc_request);
    return wire_bytes(wire);
}

inline std::vector<std::byte> official_wire(const MoveOrderRequest& request) {
    official_cgate99::MoveOrder wire;
    std::memset(&wire, 0, sizeof(wire));
    static_assert(sizeof(wire) == official_cgate99::sizeof_MoveOrder);
    assign_wire(wire.broker_code, request.broker_code);
    wire.regime = wire_scalar<decltype(wire.regime)>(request.regime);
    wire.order_id1 = wire_scalar<decltype(wire.order_id1)>(request.order_id1);
    wire.amount1 = wire_scalar<decltype(wire.amount1)>(request.amount1);
    assign_wire(wire.price1, request.price1);
    wire.ext_id1 = wire_scalar<decltype(wire.ext_id1)>(request.ext_id1);
    wire.order_id2 = wire_scalar<decltype(wire.order_id2)>(request.order_id2);
    wire.amount2 = wire_scalar<decltype(wire.amount2)>(request.amount2);
    assign_wire(wire.price2, request.price2);
    wire.ext_id2 = wire_scalar<decltype(wire.ext_id2)>(request.ext_id2);
    wire.is_check_limit = wire_scalar<decltype(wire.is_check_limit)>(request.is_check_limit);
    wire.ncc_request = wire_scalar<decltype(wire.ncc_request)>(request.ncc_request);
    assign_wire(wire.client_code, request.client_code);
    wire.isin_id = wire_scalar<decltype(wire.isin_id)>(request.isin_id);
    assign_wire(wire.compliance_id, request.compliance_id);
    return wire_bytes(wire);
}

inline std::vector<std::byte> official_wire(const IcebergMoveOrderRequest& request) {
    official_cgate99::IcebergMoveOrder wire;
    std::memset(&wire, 0, sizeof(wire));
    static_assert(sizeof(wire) == official_cgate99::sizeof_IcebergMoveOrder);
    assign_wire(wire.broker_code, request.broker_code);
    wire.order_id = wire_scalar<decltype(wire.order_id)>(request.order_id);
    wire.isin_id = wire_scalar<decltype(wire.isin_id)>(request.isin_id);
    assign_wire(wire.price, request.price);
    wire.ext_id = wire_scalar<decltype(wire.ext_id)>(request.ext_id);
    wire.ncc_request = wire_scalar<decltype(wire.ncc_request)>(request.ncc_request);
    wire.is_check_limit = wire_scalar<decltype(wire.is_check_limit)>(request.is_check_limit);
    assign_wire(wire.compliance_id, request.compliance_id);
    return wire_bytes(wire);
}

inline std::vector<std::byte> official_wire(const DelUserOrdersRequest& request) {
    official_cgate99::DelUserOrders wire;
    std::memset(&wire, 0, sizeof(wire));
    static_assert(sizeof(wire) == official_cgate99::sizeof_DelUserOrders);
    assign_wire(wire.broker_code, request.broker_code);
    wire.buy_sell = wire_scalar<decltype(wire.buy_sell)>(request.buy_sell);
    wire.non_system = wire_scalar<decltype(wire.non_system)>(request.non_system);
    assign_wire(wire.code, request.code);
    assign_wire(wire.base_contract_code, request.base_contract_code);
    wire.ext_id = wire_scalar<decltype(wire.ext_id)>(request.ext_id);
    wire.isin_id = wire_scalar<decltype(wire.isin_id)>(request.isin_id);
    wire.instrument_mask = wire_scalar<decltype(wire.instrument_mask)>(request.instrument_mask);
    return wire_bytes(wire);
}

inline std::vector<std::byte> official_wire(const DelOrdersByBFLimitRequest& request) {
    official_cgate99::DelOrdersByBFLimit wire;
    std::memset(&wire, 0, sizeof(wire));
    static_assert(sizeof(wire) == official_cgate99::sizeof_DelOrdersByBFLimit);
    assign_wire(wire.broker_code, request.broker_code);
    return wire_bytes(wire);
}

inline std::vector<std::byte> official_wire(const CODHeartbeatRequest& request) {
    official_cgate99::CODHeartbeat wire;
    std::memset(&wire, 0, sizeof(wire));
    static_assert(sizeof(wire) == official_cgate99::sizeof_CODHeartbeat);
    wire.seq_number = wire_scalar<decltype(wire.seq_number)>(request.seq_number);
    return wire_bytes(wire);
}
} // namespace moex::plaza2_trade::test_support
