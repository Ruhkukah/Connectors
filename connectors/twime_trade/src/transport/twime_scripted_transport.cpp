#include "moex/twime_trade/transport/twime_scripted_transport.hpp"

#include <algorithm>
#include <stdexcept>

namespace moex::twime_trade::transport {

TwimeTransportResult TwimeScriptedTransport::open() {
    ++metrics_.open_calls;
    if (state_ == TwimeTransportState::Open) {
        return {.status = TwimeTransportStatus::InvalidState, .event = TwimeTransportEvent::Fault};
    }

    state_ = TwimeTransportState::Opening;
    state_ = TwimeTransportState::Open;
    return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::Opened};
}

TwimeTransportResult TwimeScriptedTransport::close() {
    ++metrics_.close_calls;
    if (state_ == TwimeTransportState::Closed || state_ == TwimeTransportState::Created) {
        state_ = TwimeTransportState::Closed;
        return {.status = TwimeTransportStatus::Closed, .event = TwimeTransportEvent::Closed};
    }

    state_ = TwimeTransportState::Closing;
    state_ = TwimeTransportState::Closed;
    return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::Closed};
}

TwimeTransportResult TwimeScriptedTransport::write(std::span<const std::byte> bytes) {
    ++metrics_.write_calls;
    if (state_ != TwimeTransportState::Open) {
        return {.status = TwimeTransportStatus::InvalidState, .event = TwimeTransportEvent::Fault};
    }
    if (!write_actions_.empty()) {
        const auto action = write_actions_.front();
        write_actions_.pop_front();
        if (action == WriteActionKind::Fault) {
            state_ = TwimeTransportState::Faulted;
            ++metrics_.fault_events;
            return {.status = TwimeTransportStatus::Fault, .event = TwimeTransportEvent::Fault};
        }
        ++metrics_.write_would_block_events;
        return {.status = TwimeTransportStatus::WouldBlock, .event = TwimeTransportEvent::WriteWouldBlock};
    }
    if (bytes.empty()) {
        return {.status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::BytesWritten};
    }

    const auto write_capacity =
        max_buffered_bytes_ > written_bytes_.size() ? max_buffered_bytes_ - written_bytes_.size() : 0;
    const auto accepted = std::min({bytes.size(), max_write_size_, write_capacity});
    if (accepted == 0) {
        ++metrics_.write_would_block_events;
        return {.status = TwimeTransportStatus::WouldBlock, .event = TwimeTransportEvent::WriteWouldBlock};
    }
    written_bytes_.insert(written_bytes_.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(accepted));
    metrics_.bytes_written += accepted;
    metrics_.max_write_buffer_depth = std::max(metrics_.max_write_buffer_depth, written_bytes_.size());
    if (accepted < bytes.size()) {
        ++metrics_.partial_write_events;
        return {.status = TwimeTransportStatus::Ok,
                .event = TwimeTransportEvent::PartialWrite,
                .bytes_transferred = accepted};
    }

    return {
        .status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::BytesWritten, .bytes_transferred = accepted};
}

TwimeTransportPollResult TwimeScriptedTransport::poll_read(std::span<std::byte> out) {
    ++metrics_.read_calls;
    if (state_ != TwimeTransportState::Open) {
        return {.status = TwimeTransportStatus::InvalidState, .event = TwimeTransportEvent::Fault};
    }
    if (out.empty()) {
        return {.status = TwimeTransportStatus::BufferTooSmall, .event = TwimeTransportEvent::Fault};
    }
    if (read_actions_.empty()) {
        ++metrics_.read_would_block_events;
        return {.status = TwimeTransportStatus::WouldBlock, .event = TwimeTransportEvent::ReadWouldBlock};
    }

    auto& action = read_actions_.front();
    switch (action.kind) {
    case ReadActionKind::WouldBlock:
        read_actions_.pop_front();
        ++metrics_.read_would_block_events;
        return {.status = TwimeTransportStatus::WouldBlock, .event = TwimeTransportEvent::ReadWouldBlock};
    case ReadActionKind::RemoteClose:
        read_actions_.pop_front();
        state_ = TwimeTransportState::Closed;
        ++metrics_.remote_close_events;
        return {.status = TwimeTransportStatus::RemoteClosed, .event = TwimeTransportEvent::RemoteClose};
    case ReadActionKind::Fault:
        read_actions_.pop_front();
        state_ = TwimeTransportState::Faulted;
        ++metrics_.fault_events;
        return {.status = TwimeTransportStatus::Fault, .event = TwimeTransportEvent::Fault};
    case ReadActionKind::Bytes:
        break;
    }

    const auto remaining = action.bytes.size() - action.offset;
    const auto readable = std::min({remaining, out.size(), action.max_chunk_size});
    std::copy_n(action.bytes.data() + static_cast<std::ptrdiff_t>(action.offset), readable, out.data());
    action.offset += readable;
    queued_read_bytes_ -= readable;
    metrics_.bytes_read += readable;
    if (action.offset == action.bytes.size()) {
        read_actions_.pop_front();
        return {
            .status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::BytesRead, .bytes_transferred = readable};
    }

    ++metrics_.partial_read_events;
    return {
        .status = TwimeTransportStatus::Ok, .event = TwimeTransportEvent::PartialRead, .bytes_transferred = readable};
}

TwimeTransportState TwimeScriptedTransport::state() const noexcept {
    return state_;
}

TwimeTransportMetrics TwimeScriptedTransport::metrics() const noexcept {
    return metrics_;
}

void TwimeScriptedTransport::set_max_write_size(std::size_t max_write_size) noexcept {
    max_write_size_ = max_write_size == 0 ? 1 : max_write_size;
}

void TwimeScriptedTransport::set_max_buffered_bytes(std::size_t max_buffered_bytes) noexcept {
    max_buffered_bytes_ = max_buffered_bytes == 0 ? 1 : max_buffered_bytes;
}

void TwimeScriptedTransport::queue_read_bytes(std::span<const std::byte> bytes, std::size_t max_chunk_size) {
    if (bytes.size() > max_buffered_bytes_ - std::min(max_buffered_bytes_, queued_read_bytes_)) {
        throw std::runtime_error("queue_read_bytes exceeds scripted transport buffered-byte limit");
    }
    ReadAction action;
    action.kind = ReadActionKind::Bytes;
    action.bytes.assign(bytes.begin(), bytes.end());
    action.max_chunk_size = max_chunk_size == 0 ? 1 : max_chunk_size;
    read_actions_.push_back(std::move(action));
    queued_read_bytes_ += bytes.size();
    metrics_.max_read_buffer_depth = std::max(metrics_.max_read_buffer_depth, queued_read_bytes_);
}

void TwimeScriptedTransport::queue_read_would_block() {
    read_actions_.push_back(ReadAction{.kind = ReadActionKind::WouldBlock});
}

void TwimeScriptedTransport::queue_remote_close() {
    read_actions_.push_back(ReadAction{.kind = ReadActionKind::RemoteClose});
}

void TwimeScriptedTransport::queue_read_fault() {
    read_actions_.push_back(ReadAction{.kind = ReadActionKind::Fault});
}

void TwimeScriptedTransport::queue_write_fault() {
    write_actions_.push_back(WriteActionKind::Fault);
}

void TwimeScriptedTransport::queue_write_would_block() {
    write_actions_.push_back(WriteActionKind::WouldBlock);
}

const std::vector<std::byte>& TwimeScriptedTransport::written_bytes() const noexcept {
    return written_bytes_;
}

std::vector<std::byte> TwimeScriptedTransport::drain_written_bytes() {
    auto bytes = std::move(written_bytes_);
    written_bytes_.clear();
    return bytes;
}

} // namespace moex::twime_trade::transport
