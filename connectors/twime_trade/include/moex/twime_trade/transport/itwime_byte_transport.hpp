#pragma once

#include "moex/twime_trade/transport/twime_transport_events.hpp"
#include "moex/twime_trade/transport/twime_transport_metrics.hpp"

#include <cstddef>
#include <span>

namespace moex::twime_trade::transport {

class ITwimeByteTransport {
  public:
    virtual ~ITwimeByteTransport() = default;

    virtual TwimeTransportResult open() = 0;
    virtual TwimeTransportResult close() = 0;

    virtual TwimeTransportResult write(std::span<const std::byte> bytes) = 0;
    virtual TwimeTransportPollResult poll_read(std::span<std::byte> out) = 0;

    [[nodiscard]] virtual TwimeTransportState state() const noexcept = 0;
    [[nodiscard]] virtual TwimeTransportMetrics metrics() const noexcept = 0;
};

} // namespace moex::twime_trade::transport
