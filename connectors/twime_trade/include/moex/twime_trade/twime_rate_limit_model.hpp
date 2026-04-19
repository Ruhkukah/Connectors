#pragma once

#include "moex/twime_sbe/twime_schema.hpp"

#include <cstdint>

namespace moex::twime_trade {

enum class TwimeOutboundRateCategory {
    SessionControl,
    SessionHeartbeat,
    TradingMessage,
};

struct TwimeOutboundRateClassification {
    TwimeOutboundRateCategory category{TwimeOutboundRateCategory::SessionControl};
    bool counts_total_rate{true};
    bool counts_trading_rate{false};
    bool counts_heartbeat_rate{false};
};

struct TwimeRateLimitDecision {
    bool allowed{true};
    bool total_rate_violation{false};
    bool trading_rate_violation{false};
    bool heartbeat_rate_violation{false};
    std::uint32_t total_used_in_window{0};
    std::uint32_t trading_used_in_window{0};
    std::uint32_t heartbeat_used_in_window{0};
    std::uint64_t retry_after_ms{0};
};

class TwimeRateLimitModel {
  public:
    TwimeRateLimitModel(std::uint32_t max_total_messages_per_window = 1024,
                        std::uint32_t max_trading_messages_per_window = 1024,
                        std::uint32_t max_heartbeats_per_window = 3, std::uint64_t window_ms = 1000) noexcept;

    [[nodiscard]] TwimeRateLimitDecision observe_send(std::uint64_t now_ms,
                                                      TwimeOutboundRateClassification classification) noexcept;
    void reset() noexcept;

  private:
    std::uint32_t max_total_messages_per_window_{1024};
    std::uint32_t max_trading_messages_per_window_{1024};
    std::uint32_t max_heartbeats_per_window_{3};
    std::uint64_t window_ms_{1000};
    std::uint64_t window_start_ms_{0};
    std::uint32_t total_used_in_window_{0};
    std::uint32_t trading_used_in_window_{0};
    std::uint32_t heartbeat_used_in_window_{0};
};

[[nodiscard]] TwimeOutboundRateClassification
classify_outbound_message(const moex::twime_sbe::TwimeMessageMetadata& metadata) noexcept;

} // namespace moex::twime_trade
