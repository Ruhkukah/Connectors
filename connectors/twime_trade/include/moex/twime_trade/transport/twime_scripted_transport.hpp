#pragma once

#include "moex/twime_trade/transport/itwime_byte_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <span>
#include <vector>

namespace moex::twime_trade::transport {

class TwimeScriptedTransport final : public ITwimeByteTransport {
  public:
    TwimeTransportResult open() override;
    TwimeTransportResult close() override;
    TwimeTransportResult write(std::span<const std::byte> bytes) override;
    TwimeTransportPollResult poll_read(std::span<std::byte> out) override;

    [[nodiscard]] TwimeTransportState state() const noexcept override;
    [[nodiscard]] TwimeTransportMetrics metrics() const noexcept override;

    void set_max_write_size(std::size_t max_write_size) noexcept;
    void set_max_buffered_bytes(std::size_t max_buffered_bytes) noexcept;
    void queue_read_bytes(std::span<const std::byte> bytes,
                          std::size_t max_chunk_size = std::numeric_limits<std::size_t>::max());
    void queue_read_would_block();
    void queue_remote_close();
    void queue_read_fault();
    void queue_write_fault();
    void queue_write_would_block();

    [[nodiscard]] const std::vector<std::byte>& written_bytes() const noexcept;
    [[nodiscard]] std::vector<std::byte> drain_written_bytes();

  private:
    enum class ReadActionKind {
        Bytes,
        WouldBlock,
        RemoteClose,
        Fault,
    };

    enum class WriteActionKind {
        WouldBlock,
        Fault,
    };

    struct ReadAction {
        ReadActionKind kind{ReadActionKind::WouldBlock};
        std::vector<std::byte> bytes{};
        std::size_t offset{0};
        std::size_t max_chunk_size{std::numeric_limits<std::size_t>::max()};
    };

    TwimeTransportState state_{TwimeTransportState::Created};
    TwimeTransportMetrics metrics_{};
    std::size_t max_write_size_{std::numeric_limits<std::size_t>::max()};
    std::size_t max_buffered_bytes_{1U << 20};
    std::size_t queued_read_bytes_{0};
    std::deque<ReadAction> read_actions_{};
    std::deque<WriteActionKind> write_actions_{};
    std::vector<std::byte> written_bytes_{};
};

} // namespace moex::twime_trade::transport
