#include "moex/twime_trade/transport/twime_loopback_transport.hpp"

#include <algorithm>
#include <stdexcept>

namespace moex::twime_trade::transport {

TwimeTransportResult TwimeLoopbackTransport::open() {
    ++metrics_.open_calls;
    if (state_ == TwimeTransportState::Open) {
        return {.status = TwimeTransportStatus::InvalidState, .event = TwimeTransportEvent::Fault};
    }

    state_ = TwimeTransportState::Opening;
    state_ = TwimeTransportState::Open;
    return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::Opened};
}

TwimeTransportResult TwimeLoopbackTransport::close() {
    ++metrics_.close_calls;
    if (state_ == TwimeTransportState::Closed || state_ == TwimeTransportState::Created) {
        state_ = TwimeTransportState::Closed;
        return {.status = TwimeTransportStatus::Closed, .event = TwimeTransportEvent::Closed};
    }

    state_ = TwimeTransportState::Closing;
    state_ = TwimeTransportState::Closed;
    return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::Closed};
}

TwimeTransportResult TwimeLoopbackTransport::write(std::span<const std::byte> bytes) {
    ++metrics_.write_calls;
    if (state_ != TwimeTransportState::Open) {
        return {.status = TwimeTransportStatus::InvalidState, .event = TwimeTransportEvent::Fault};
    }
    if (next_write_fault_) {
        next_write_fault_ = false;
        state_ = TwimeTransportState::Faulted;
        ++metrics_.fault_events;
        return {.status = TwimeTransportStatus::Fault, .event = TwimeTransportEvent::Fault};
    }
    if (bytes.empty()) {
        return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::BytesWritten};
    }

    const auto readable_capacity =
        max_buffered_bytes_ > readable_bytes_.size() ? max_buffered_bytes_ - readable_bytes_.size() : 0;
    const auto write_capacity =
        max_buffered_bytes_ > written_bytes_.size() ? max_buffered_bytes_ - written_bytes_.size() : 0;
    const auto accepted = std::min({bytes.size(), max_write_size_, readable_capacity, write_capacity});
    if (accepted == 0) {
        ++metrics_.write_would_block_events;
        return {.status = TwimeTransportStatus::WouldBlock, .event = TwimeTransportEvent::WriteWouldBlock};
    }
    written_bytes_.insert(written_bytes_.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(accepted));
    readable_bytes_.insert(readable_bytes_.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(accepted));
    metrics_.bytes_written += accepted;
    metrics_.max_read_buffer_depth = std::max(metrics_.max_read_buffer_depth, readable_bytes_.size());
    metrics_.max_write_buffer_depth = std::max(metrics_.max_write_buffer_depth, written_bytes_.size());
    if (accepted < bytes.size()) {
        ++metrics_.partial_write_events;
        return {
            .status = TwimeTransportStatus::Ok,
            .event = TwimeTransportEvent::PartialWrite,
            .bytes_transferred = accepted,
        };
    }

    return {
        .status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::BytesWritten, .bytes_transferred = accepted};
}

TwimeTransportPollResult TwimeLoopbackTransport::poll_read(std::span<std::byte> out) {
    ++metrics_.read_calls;
    if (state_ != TwimeTransportState::Open) {
        return {.status = TwimeTransportStatus::InvalidState, .event = TwimeTransportEvent::Fault};
    }
    if (next_read_fault_) {
        next_read_fault_ = false;
        state_ = TwimeTransportState::Faulted;
        ++metrics_.fault_events;
        return {.status = TwimeTransportStatus::Fault, .event = TwimeTransportEvent::Fault};
    }
    if (remote_close_pending_ && readable_bytes_.empty()) {
        remote_close_pending_ = false;
        state_ = TwimeTransportState::Closed;
        ++metrics_.remote_close_events;
        return {.status = TwimeTransportStatus::RemoteClosed, .event = TwimeTransportEvent::RemoteClose};
    }
    if (out.empty()) {
        return {.status = TwimeTransportStatus::BufferTooSmall, .event = TwimeTransportEvent::Fault};
    }
    if (readable_bytes_.empty()) {
        ++metrics_.read_would_block_events;
        return {.status = TwimeTransportStatus::WouldBlock, .event = TwimeTransportEvent::ReadWouldBlock};
    }

    const auto readable = std::min({out.size(), readable_bytes_.size(), max_read_size_});
    for (std::size_t index = 0; index < readable; ++index) {
        out[index] = readable_bytes_.front();
        readable_bytes_.pop_front();
    }
    metrics_.bytes_read += readable;
    if (readable < out.size() && !readable_bytes_.empty()) {
        ++metrics_.partial_read_events;
        return {.status = TwimeTransportStatus::Ok,
                .event = TwimeTransportEvent::PartialRead,
                .bytes_transferred = readable};
    }
    if (readable == max_read_size_ && !readable_bytes_.empty()) {
        ++metrics_.partial_read_events;
        return {.status = TwimeTransportStatus::Ok,
                .event = TwimeTransportEvent::PartialRead,
                .bytes_transferred = readable};
    }

    return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::BytesRead, .bytes_transferred = readable};
}

TwimeTransportState TwimeLoopbackTransport::state() const noexcept {
    return state_;
}

TwimeTransportMetrics TwimeLoopbackTransport::metrics() const noexcept {
    return metrics_;
}

void TwimeLoopbackTransport::set_max_read_size(std::size_t max_read_size) noexcept {
    max_read_size_ = max_read_size == 0 ? 1 : max_read_size;
}

void TwimeLoopbackTransport::set_max_write_size(std::size_t max_write_size) noexcept {
    max_write_size_ = max_write_size == 0 ? 1 : max_write_size;
}

void TwimeLoopbackTransport::set_max_buffered_bytes(std::size_t max_buffered_bytes) noexcept {
    max_buffered_bytes_ = max_buffered_bytes == 0 ? 1 : max_buffered_bytes;
}

void TwimeLoopbackTransport::inject_next_read_fault() noexcept {
    next_read_fault_ = true;
}

void TwimeLoopbackTransport::inject_next_write_fault() noexcept {
    next_write_fault_ = true;
}

void TwimeLoopbackTransport::script_remote_close() noexcept {
    remote_close_pending_ = true;
}

void TwimeLoopbackTransport::queue_inbound_bytes(std::span<const std::byte> bytes) {
    if (bytes.size() > max_buffered_bytes_ - std::min(max_buffered_bytes_, readable_bytes_.size())) {
        throw std::runtime_error("queue_inbound_bytes exceeds loopback buffered-byte limit");
    }
    readable_bytes_.insert(readable_bytes_.end(), bytes.begin(), bytes.end());
    metrics_.max_read_buffer_depth = std::max(metrics_.max_read_buffer_depth, readable_bytes_.size());
}

const std::vector<std::byte>& TwimeLoopbackTransport::written_bytes() const noexcept {
    return written_bytes_;
}

std::vector<std::byte> TwimeLoopbackTransport::drain_written_bytes() {
    auto bytes = std::move(written_bytes_);
    written_bytes_.clear();
    return bytes;
}

} // namespace moex::twime_trade::transport
