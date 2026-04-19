#pragma once

#include <chrono>

namespace moex::twime_trade::transport {

struct TwimeReconnectPolicy {
    std::chrono::milliseconds min_reconnect_delay{1000};
    std::chrono::milliseconds max_reconnect_delay{5000};
    bool exponential_backoff{false};
    bool jitter{false};
};

[[nodiscard]] bool twime_reconnect_allowed(std::chrono::steady_clock::time_point now,
                                           std::chrono::steady_clock::time_point last_attempt,
                                           const TwimeReconnectPolicy& policy) noexcept;

[[nodiscard]] std::chrono::steady_clock::time_point
twime_next_reconnect_time(std::chrono::steady_clock::time_point last_attempt,
                          const TwimeReconnectPolicy& policy) noexcept;

} // namespace moex::twime_trade::transport
