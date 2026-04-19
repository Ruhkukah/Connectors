#include "moex/twime_trade/twime_fake_transport.hpp"

namespace moex::twime_trade {

void TwimeFakeTransport::connect() noexcept {
    connected_ = true;
}

void TwimeFakeTransport::disconnect() noexcept {
    connected_ = false;
}

bool TwimeFakeTransport::connected() const noexcept {
    return connected_;
}

void TwimeFakeTransport::script_inbound_frame(const TwimeFakeTransportFrame& frame) {
    inbound_.push_back(TwimeFakeTransportEvent{TwimeFakeTransportEventKind::Frame, frame});
}

void TwimeFakeTransport::script_peer_close() {
    inbound_.push_back(TwimeFakeTransportEvent{TwimeFakeTransportEventKind::PeerClosed, {}});
}

std::vector<TwimeFakeTransportEvent> TwimeFakeTransport::drain_inbound() {
    std::vector<TwimeFakeTransportEvent> events(inbound_.begin(), inbound_.end());
    inbound_.clear();
    return events;
}

void TwimeFakeTransport::send_outbound_frame(const TwimeFakeTransportFrame& frame) {
    outbound_.push_back(frame);
}

const std::deque<TwimeFakeTransportFrame>& TwimeFakeTransport::outbound_frames() const noexcept {
    return outbound_;
}

std::vector<TwimeFakeTransportFrame> TwimeFakeTransport::drain_outbound() {
    std::vector<TwimeFakeTransportFrame> frames(outbound_.begin(), outbound_.end());
    outbound_.clear();
    return frames;
}

void TwimeFakeTransport::clear() noexcept {
    inbound_.clear();
    outbound_.clear();
    connected_ = false;
}

}  // namespace moex::twime_trade
