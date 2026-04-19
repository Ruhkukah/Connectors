#pragma once

#include <cstdint>
#include <string>

namespace moex::reconciliation {

enum class SourcePriority : std::uint8_t { Primary = 1, Secondary = 2, Fallback = 3 };

struct CorrelationKey {
    std::string cl_ord_id;
    std::string exchange_order_id;
    std::string trade_id;
    std::string account;
    std::string security_id;
    std::string source_connector;
    std::uint64_t source_sequence = 0;
    std::string recovery_epoch;
};

} // namespace moex::reconciliation
