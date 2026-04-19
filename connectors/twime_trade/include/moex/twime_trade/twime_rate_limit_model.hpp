#pragma once

#include <cstdint>

namespace moex::twime_trade {

struct TwimeRateLimitDecision {
    bool allowed{true};
    std::uint32_t used_in_window{0};
    std::uint64_t retry_after_ms{0};
};

class TwimeRateLimitModel {
  public:
    TwimeRateLimitModel(std::uint32_t max_messages_per_window = 1024, std::uint64_t window_ms = 1000) noexcept;

    [[nodiscard]] TwimeRateLimitDecision observe_send(std::uint64_t now_ms) noexcept;
    void reset() noexcept;

  private:
    std::uint32_t max_messages_per_window_{1024};
    std::uint64_t window_ms_{1000};
    std::uint64_t window_start_ms_{0};
    std::uint32_t used_in_window_{0};
};

}  // namespace moex::twime_trade
