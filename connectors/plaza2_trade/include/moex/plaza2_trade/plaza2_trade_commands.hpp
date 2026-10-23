#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace moex::plaza2_trade {

enum class Plaza2TradeCommandKind : std::uint16_t {
    AddOrder = 474,
    DelOrder = 461,
    MoveOrder = 476,
    DelUserOrders = 466,
};

enum class Plaza2TradeSide : std::int32_t {
    Buy = 1,
    Sell = 2,
};

enum class Plaza2TradeOrderType : std::int32_t {
    Limit = 1,
    Ioc = 2,
};

struct AddOrderRequest {
    std::optional<std::string> broker_code;
    std::optional<std::int32_t> isin_id;
    std::optional<std::string> client_code;
    std::optional<Plaza2TradeSide> dir;
    std::optional<Plaza2TradeOrderType> type;
    std::optional<std::int32_t> amount;
    std::optional<std::string> price;
    std::optional<std::string> comment;
    std::optional<std::string> broker_to;
    std::optional<std::int32_t> ext_id;
    std::optional<std::int32_t> is_check_limit;
    std::optional<std::string> date_exp;
    std::optional<std::int32_t> dont_check_money;
    std::optional<std::string> match_ref;
    std::optional<std::int8_t> ncc_request;
    std::optional<std::string> compliance_id;
};

struct DelOrderRequest {
    std::optional<std::string> broker_code;
    std::optional<std::int64_t> order_id;
    std::optional<std::int8_t> ncc_request;
    std::optional<std::string> client_code;
    std::optional<std::int32_t> isin_id;
};

struct MoveOrderRequest {
    std::optional<std::string> broker_code;
    std::optional<std::int32_t> regime;
    std::optional<std::int64_t> order_id1;
    std::optional<std::int32_t> amount1;
    std::optional<std::string> price1;
    std::optional<std::int32_t> ext_id1;
    std::optional<std::int64_t> order_id2;
    std::optional<std::int32_t> amount2;
    std::optional<std::string> price2;
    std::optional<std::int32_t> ext_id2;
    std::optional<std::int32_t> is_check_limit;
    std::optional<std::int8_t> ncc_request;
    std::optional<std::string> client_code;
    std::optional<std::int32_t> isin_id;
    std::optional<std::string> compliance_id;
};

struct DelUserOrdersRequest {
    std::optional<std::string> broker_code;
    std::optional<std::int32_t> buy_sell;
    std::optional<std::int32_t> non_system;
    std::optional<std::string> code;
    std::optional<std::string> base_contract_code;
    std::optional<std::int32_t> ext_id;
    std::optional<std::int32_t> isin_id;
    std::optional<std::int8_t> instrument_mask;
};

using Plaza2TradeCommandRequest =
    std::variant<AddOrderRequest, DelOrderRequest, MoveOrderRequest, DelUserOrdersRequest>;

[[nodiscard]] Plaza2TradeCommandKind command_kind(const Plaza2TradeCommandRequest& request);
[[nodiscard]] const char* command_name(Plaza2TradeCommandKind kind) noexcept;
[[nodiscard]] std::int32_t command_msgid(Plaza2TradeCommandKind kind) noexcept;

} // namespace moex::plaza2_trade
