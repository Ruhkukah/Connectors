#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace moex::twime_trade {

struct TwimeFakeTransportFrame {
    std::vector<std::byte> bytes;
    std::uint64_t sequence_number{0};
    bool consumes_sequence{false};
};

enum class TwimeFakeTransportEventKind {
    Frame,
    PeerClosed,
};

struct TwimeFakeTransportEvent {
    TwimeFakeTransportEventKind kind{TwimeFakeTransportEventKind::Frame};
    TwimeFakeTransportFrame frame{};
};

class TwimeFakeTransport {
  public:
    void connect() noexcept;
    void disconnect() noexcept;
    [[nodiscard]] bool connected() const noexcept;

    void script_inbound_frame(const TwimeFakeTransportFrame& frame);
    void script_peer_close();
    [[nodiscard]] std::vector<TwimeFakeTransportEvent> drain_inbound();

    void send_outbound_frame(const TwimeFakeTransportFrame& frame);
    [[nodiscard]] const std::deque<TwimeFakeTransportFrame>& outbound_frames() const noexcept;
    [[nodiscard]] std::vector<TwimeFakeTransportFrame> drain_outbound();
    void clear() noexcept;

  private:
    bool connected_{false};
    std::deque<TwimeFakeTransportEvent> inbound_;
    std::deque<TwimeFakeTransportFrame> outbound_;
};

}  // namespace moex::twime_trade
