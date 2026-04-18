#include "moex/twime_sbe/twime_frame_assembler.hpp"

#include "moex/twime_sbe/twime_schema.hpp"

#include <algorithm>

namespace moex::twime_sbe {

namespace {

TwimeDecodeError validate_header(
    const TwimeMessageHeader& header,
    std::size_t max_frame_size,
    std::size_t& out_frame_size) {
    if (header.schema_id != TwimeSchemaView::info().schema_id) {
        return TwimeDecodeError::UnsupportedSchemaId;
    }
    if (header.version != TwimeSchemaView::info().schema_version) {
        return TwimeDecodeError::UnsupportedVersion;
    }
    const auto* metadata = TwimeSchemaView::find_message_by_template_id(header.template_id);
    if (metadata == nullptr) {
        return TwimeDecodeError::UnknownTemplateId;
    }
    if (header.block_length != metadata->block_length || header.block_length == 0) {
        return TwimeDecodeError::InvalidBlockLength;
    }

    const auto frame_size = kTwimeMessageHeaderSize + static_cast<std::size_t>(header.block_length);
    if (frame_size > max_frame_size) {
        return TwimeDecodeError::BufferTooSmall;
    }
    out_frame_size = frame_size;
    return TwimeDecodeError::Ok;
}

void clear_partial_state(std::vector<std::byte>& buffer) {
    buffer.clear();
}

}  // namespace

TwimeFrameAssembler::TwimeFrameAssembler(std::size_t max_frame_size)
    : max_frame_size_(max_frame_size) {}

TwimeFrameFeedResult TwimeFrameAssembler::feed(std::span<const std::byte> bytes) {
    TwimeFrameFeedResult result{};
    std::size_t offset = 0;

    while (offset < bytes.size()) {
        if (buffer_.empty()) {
            const auto remaining = bytes.subspan(offset);
            if (remaining.size() < kTwimeMessageHeaderSize) {
                buffer_.assign(remaining.begin(), remaining.end());
                offset = bytes.size();
                break;
            }

            TwimeMessageHeader header{};
            const auto header_error = decode_twime_message_header(remaining.first(kTwimeMessageHeaderSize), header);
            if (header_error != TwimeDecodeError::Ok) {
                result.error = header_error;
                clear_partial_state(buffer_);
                result.buffered_bytes = 0;
                return result;
            }

            std::size_t frame_size = 0;
            const auto validation_error = validate_header(header, max_frame_size_, frame_size);
            if (validation_error != TwimeDecodeError::Ok) {
                result.error = validation_error;
                clear_partial_state(buffer_);
                result.buffered_bytes = 0;
                return result;
            }

            if (remaining.size() >= frame_size) {
                ready_frames_.emplace_back(remaining.begin(), remaining.begin() + static_cast<std::ptrdiff_t>(frame_size));
                offset += frame_size;
                ++result.frames_ready;
                continue;
            }

            buffer_.reserve(frame_size);
            buffer_.assign(remaining.begin(), remaining.end());
            offset = bytes.size();
            break;
        }

        if (buffer_.size() < kTwimeMessageHeaderSize) {
            const auto need_for_header = kTwimeMessageHeaderSize - buffer_.size();
            const auto to_copy = std::min<std::size_t>(need_for_header, bytes.size() - offset);
            buffer_.insert(buffer_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                           bytes.begin() + static_cast<std::ptrdiff_t>(offset + to_copy));
            offset += to_copy;
            if (buffer_.size() < kTwimeMessageHeaderSize) {
                break;
            }
        }

        TwimeMessageHeader header{};
        const auto header_error = decode_twime_message_header(
            std::span<const std::byte>(buffer_.data(), kTwimeMessageHeaderSize),
            header);
        if (header_error != TwimeDecodeError::Ok) {
            result.error = header_error;
            clear_partial_state(buffer_);
            result.buffered_bytes = 0;
            return result;
        }

        std::size_t frame_size = 0;
        const auto validation_error = validate_header(header, max_frame_size_, frame_size);
        if (validation_error != TwimeDecodeError::Ok) {
            result.error = validation_error;
            clear_partial_state(buffer_);
            result.buffered_bytes = 0;
            return result;
        }

        const auto missing = frame_size - buffer_.size();
        const auto to_copy = std::min<std::size_t>(missing, bytes.size() - offset);
        buffer_.insert(
            buffer_.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset + to_copy));
        offset += to_copy;

        if (buffer_.size() < frame_size) {
            break;
        }

        ready_frames_.push_back(std::move(buffer_));
        buffer_.clear();
        ++result.frames_ready;
    }

    result.error = result.frames_ready == 0 && !buffer_.empty() ? TwimeDecodeError::NeedMoreData : TwimeDecodeError::Ok;
    result.buffered_bytes = buffer_.size();
    return result;
}

bool TwimeFrameAssembler::has_frame() const noexcept {
    return !ready_frames_.empty();
}

TwimeFrameView TwimeFrameAssembler::pop_frame() {
    if (ready_frames_.empty()) {
        return {};
    }
    auto bytes = std::move(ready_frames_.front());
    ready_frames_.pop_front();
    return TwimeFrameView{std::move(bytes)};
}

void TwimeFrameAssembler::reset() {
    buffer_.clear();
    ready_frames_.clear();
}

}  // namespace moex::twime_sbe
