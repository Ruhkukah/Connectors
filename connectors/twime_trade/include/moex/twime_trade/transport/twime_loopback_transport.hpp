#pragma once

#include "moex/twime_trade/transport/itwime_byte_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace moex::twime_trade::transport {

class TwimeLoopbackTransport final : public ITwimeByteTransport {
  public:
    TwimeTransportResult open() override;
    TwimeTransportResult close() override;
    TwimeTransportResult write(std::span<const std::byte> bytes) override;
    TwimeTransportPollResult poll_read(std::span<std::byte> out) override;

    [[nodiscard]] TwimeTransportState state() const noexcept override;
    [[nodiscard]] TwimeTransportMetrics metrics() const noexcept override;

    void set_max_read_size(std::size_t max_read_size) noexcept;
    void set_max_write_size(std::size_t max_write_size) noexcept;
    void set_max_buffered_bytes(std::size_t max_buffered_bytes) noexcept;
    void inject_next_read_fault() noexcept;
    void inject_next_write_fault() noexcept;
    void script_remote_close() noexcept;
    void queue_inbound_bytes(std::span<const std::byte> bytes);

    [[nodiscard]] const std::vector<std::byte>& written_bytes() const noexcept;
    [[nodiscard]] std::vector<std::byte> drain_written_bytes();

  private:
    TwimeTransportState state_{TwimeTransportState::Created};
    TwimeTransportMetrics metrics_{};
    std::deque<std::byte> readable_bytes_{};
    std::vector<std::byte> written_bytes_{};
    std::size_t max_read_size_{std::numeric_limits<std::size_t>::max()};
    std::size_t max_write_size_{std::numeric_limits<std::size_t>::max()};
    std::size_t max_buffered_bytes_{1U << 20};
    bool next_read_fault_{false};
    bool next_write_fault_{false};
    bool remote_close_pending_{false};
};

} // namespace moex::twime_trade::transport
