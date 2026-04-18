#include "moex/twime_sbe/twime_frame_assembler.hpp"

#include "moex/twime_sbe/twime_schema.hpp"

#include <algorithm>

namespace moex::twime_sbe {

TwimeFrameAssembler::TwimeFrameAssembler(std::size_t max_frame_size)
    : max_frame_size_(max_frame_size) {}

TwimeFrameFeedResult TwimeFrameAssembler::feed(std::span<const std::byte> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    TwimeFrameFeedResult result{};
    while (true) {
        if (buffer_.size() < kTwimeMessageHeaderSize) {
            result.error = result.frames_ready == 0 ? TwimeDecodeError::NeedMoreData : TwimeDecodeError::Ok;
            result.buffered_bytes = buffer_.size();
            return result;
        }

        TwimeMessageHeader header{};
        const auto header_error = decode_twime_message_header(
            std::span<const std::byte>(buffer_.data(), kTwimeMessageHeaderSize),
            header);
        if (header_error != TwimeDecodeError::Ok) {
            result.error = header_error;
            result.buffered_bytes = buffer_.size();
            return result;
        }

        if (header.schema_id != TwimeSchemaView::info().schema_id) {
            result.error = TwimeDecodeError::UnsupportedSchemaId;
            result.buffered_bytes = buffer_.size();
            return result;
        }
        if (header.version != TwimeSchemaView::info().schema_version) {
            result.error = TwimeDecodeError::UnsupportedVersion;
            result.buffered_bytes = buffer_.size();
            return result;
        }

        const auto* metadata = TwimeSchemaView::find_message_by_template_id(header.template_id);
        if (metadata == nullptr) {
            result.error = TwimeDecodeError::UnknownTemplateId;
            result.buffered_bytes = buffer_.size();
            return result;
        }
        if (header.block_length != metadata->block_length || header.block_length == 0) {
            result.error = TwimeDecodeError::InvalidBlockLength;
            result.buffered_bytes = buffer_.size();
            return result;
        }

        const auto frame_size = kTwimeMessageHeaderSize + static_cast<std::size_t>(header.block_length);
        if (frame_size > max_frame_size_) {
            result.error = TwimeDecodeError::BufferTooSmall;
            result.buffered_bytes = buffer_.size();
            return result;
        }
        if (buffer_.size() < frame_size) {
            result.error = result.frames_ready == 0 ? TwimeDecodeError::NeedMoreData : TwimeDecodeError::Ok;
            result.buffered_bytes = buffer_.size();
            return result;
        }

        ready_frames_.emplace_back(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));
        ++result.frames_ready;
    }
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
