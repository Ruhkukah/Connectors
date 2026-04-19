#pragma once

#include "moex/twime_trade/transport/itwime_byte_transport.hpp"
#include "moex/twime_trade/transport/twime_reconnect_policy.hpp"
#include "moex/twime_trade/transport/twime_socket_handle.hpp"
#include "moex/twime_trade/transport/twime_tcp_config.hpp"

#include <chrono>
#include <functional>
#include <span>
#include <vector>

namespace moex::twime_trade::transport {

class TwimeTcpTransport final : public ITwimeByteTransport {
  public:
    explicit TwimeTcpTransport(TwimeTcpConfig config = {});

    TwimeTransportResult open() override;
    TwimeTransportResult close() override;
    TwimeTransportResult write(std::span<const std::byte> bytes) override;
    TwimeTransportPollResult poll_read(std::span<std::byte> out) override;

    [[nodiscard]] TwimeTransportState state() const noexcept override;
    [[nodiscard]] TwimeTransportMetrics metrics() const noexcept override;

    [[nodiscard]] const TwimeTcpConfig& config() const noexcept;
    void set_time_source(std::function<std::chrono::steady_clock::time_point()> time_source);

  private:
    using TimePoint = std::chrono::steady_clock::time_point;

    [[nodiscard]] TimePoint now() const;
    [[nodiscard]] TwimeTransportResult fail_open(TwimeTransportErrorCode error_code, int os_error,
                                                 TwimeTransportEvent event = TwimeTransportEvent::OpenFailed);
    [[nodiscard]] TwimeTransportResult validate_open_request() const;
    [[nodiscard]] TwimeTransportResult complete_nonblocking_connect();
    [[nodiscard]] TwimeTransportResult finalize_connect_from_poll();
    [[nodiscard]] std::size_t capped_read_size(std::size_t requested) const noexcept;
    [[nodiscard]] std::size_t capped_write_size(std::size_t requested) const noexcept;

    TwimeTcpConfig config_;
    TwimeSocketHandle socket_;
    TwimeTransportState state_{TwimeTransportState::Created};
    TwimeTransportMetrics metrics_{};
    std::function<TimePoint()> time_source_{};
    TimePoint last_open_attempt_{};
    bool has_last_open_attempt_{false};
};

} // namespace moex::twime_trade::transport
