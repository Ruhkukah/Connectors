#include "moex/twime_trade/twime_session_health.hpp"

namespace moex::twime_trade {

TwimeSessionHealthSnapshot make_twime_session_health_snapshot(const TwimeSession& session, bool transport_open,
                                                             const transport::TwimeTransportMetrics& transport_metrics,
                                                             const TwimeSessionMetrics& session_metrics) {
    TwimeSessionHealthSnapshot snapshot;
    snapshot.state = session.state();
    snapshot.transport_open = transport_open;
    snapshot.session_active = session.state() == TwimeSessionState::Active;
    snapshot.reject_seen = session.last_reject_code().has_value();
    snapshot.last_reject_code = session.last_reject_code().value_or(0);
    snapshot.next_expected_inbound_seq = session.sequence_state().next_expected_inbound_seq();
    snapshot.next_outbound_seq = session.sequence_state().next_outbound_seq();
    snapshot.active_keepalive_interval_ms = session.active_keepalive_interval_ms();
    snapshot.reconnect_attempts = session_metrics.reconnect_attempts;
    snapshot.bytes_read = transport_metrics.bytes_read;
    snapshot.bytes_written = transport_metrics.bytes_written;
    snapshot.heartbeat_sent = session_metrics.heartbeat_sent;
    snapshot.heartbeat_received = session_metrics.heartbeat_received;
    snapshot.faults = session_metrics.faults + transport_metrics.fault_events;
    snapshot.remote_closes = session_metrics.remote_closes + transport_metrics.remote_close_events;
    snapshot.last_transition_time_ms = session_metrics.last_transition_time_ms;
    return snapshot;
}

} // namespace moex::twime_trade
