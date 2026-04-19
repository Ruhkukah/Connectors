#include "moex/twime_trade/twime_session_metrics.hpp"

namespace moex::twime_trade {

void update_twime_session_metrics(TwimeSessionMetrics& metrics, std::span<const TwimeSessionEvent> events,
                                  std::uint64_t now_ms) {
    for (const auto& event : events) {
        switch (event.type) {
        case TwimeSessionEventType::StateChanged:
            ++metrics.state_changes;
            metrics.last_transition_time_ms = now_ms;
            break;
        case TwimeSessionEventType::OutboundMessage:
            if (event.message_name == "Establish") {
                ++metrics.establish_sent;
            } else if (event.message_name == "Terminate") {
                ++metrics.terminate_sent;
            }
            break;
        case TwimeSessionEventType::HeartbeatSent:
            ++metrics.heartbeat_sent;
            break;
        case TwimeSessionEventType::HeartbeatReceived:
            ++metrics.heartbeat_received;
            break;
        case TwimeSessionEventType::EstablishmentRejected:
            ++metrics.reject_events;
            break;
        case TwimeSessionEventType::TerminateReceived:
            ++metrics.terminate_received;
            break;
        case TwimeSessionEventType::Faulted:
            ++metrics.faults;
            break;
        case TwimeSessionEventType::PeerClosed:
        case TwimeSessionEventType::PeerClosedCleanAfterTerminateResponse:
        case TwimeSessionEventType::PeerClosedUnexpectedWhileTerminating:
            ++metrics.remote_closes;
            break;
        default:
            break;
        }

        if (event.message_name == "EstablishmentAck" && event.type == TwimeSessionEventType::InboundMessage) {
            ++metrics.ack_received;
        }
    }
}

} // namespace moex::twime_trade
