#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace moex::plaza2_trade {

enum class Plaza2TradeReplyStatusCategory {
    Accepted,
    Rejected,
    BusinessRejection,
    SystemError,
    Unknown,
};

struct Plaza2TradeDecodedReply {
    std::int32_t msgid{0};
    std::string message_name;
    Plaza2TradeReplyStatusCategory status{Plaza2TradeReplyStatusCategory::Unknown};
    std::int32_t code{0};
    std::optional<std::int32_t> queue_size;
    std::optional<std::int32_t> penalty_remain;
    std::optional<std::int32_t> amount;
    std::optional<std::int32_t> num_orders;
    std::optional<std::int64_t> order_id;
    std::optional<std::int64_t> iceberg_order_id;
    std::optional<std::int64_t> order_id1;
    std::optional<std::int64_t> order_id2;
    std::string message;
};

} // namespace moex::plaza2_trade
