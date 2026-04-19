#pragma once

#include "moex/twime_trade/twime_session.hpp"

#include <cstdint>
#include <span>

namespace moex::twime_trade {

struct TwimeSessionMetrics {
    std::uint64_t reconnect_attempts{0};
    std::uint64_t heartbeat_sent{0};
    std::uint64_t heartbeat_received{0};
    std::uint64_t faults{0};
    std::uint64_t remote_closes{0};
    std::uint64_t reject_events{0};
    std::uint64_t terminate_sent{0};
    std::uint64_t terminate_received{0};
    std::uint64_t establish_sent{0};
    std::uint64_t ack_received{0};
    std::uint64_t state_changes{0};
    std::uint64_t last_transition_time_ms{0};
};

void update_twime_session_metrics(TwimeSessionMetrics& metrics, std::span<const TwimeSessionEvent> events,
                                  std::uint64_t now_ms);

} // namespace moex::twime_trade
