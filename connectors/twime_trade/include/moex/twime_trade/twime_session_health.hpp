#pragma once

#include "moex/twime_trade/transport/twime_transport_metrics.hpp"
#include "moex/twime_trade/twime_session.hpp"
#include "moex/twime_trade/twime_session_metrics.hpp"

#include <cstdint>

namespace moex::twime_trade {

struct TwimeSessionHealthSnapshot {
    TwimeSessionState state{TwimeSessionState::Created};
    bool transport_open{false};
    bool session_active{false};
    bool reject_seen{false};
    std::int64_t last_reject_code{0};
    std::uint64_t next_expected_inbound_seq{1};
    std::uint64_t next_outbound_seq{1};
    std::uint32_t active_keepalive_interval_ms{0};
    std::uint64_t reconnect_attempts{0};
    std::uint64_t bytes_read{0};
    std::uint64_t bytes_written{0};
    std::uint64_t heartbeat_sent{0};
    std::uint64_t heartbeat_received{0};
    std::uint64_t faults{0};
    std::uint64_t remote_closes{0};
    std::uint64_t last_transition_time_ms{0};
};

TwimeSessionHealthSnapshot make_twime_session_health_snapshot(const TwimeSession& session, bool transport_open,
                                                              const transport::TwimeTransportMetrics& transport_metrics,
                                                              const TwimeSessionMetrics& session_metrics);

} // namespace moex::twime_trade
