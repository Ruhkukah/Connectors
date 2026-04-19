#include "moex/twime_trade/transport/twime_reconnect_policy.hpp"

namespace moex::twime_trade::transport {

bool twime_reconnect_allowed(std::chrono::steady_clock::time_point now,
                             std::chrono::steady_clock::time_point last_attempt,
                             const TwimeReconnectPolicy& policy) noexcept {
    return now - last_attempt >= policy.min_reconnect_delay;
}

std::chrono::steady_clock::time_point twime_next_reconnect_time(std::chrono::steady_clock::time_point last_attempt,
                                                                const TwimeReconnectPolicy& policy) noexcept {
    return last_attempt + policy.min_reconnect_delay;
}

} // namespace moex::twime_trade::transport
