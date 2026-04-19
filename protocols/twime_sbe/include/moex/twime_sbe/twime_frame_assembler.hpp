#pragma once

#include "moex/twime_sbe/twime_message_header.hpp"

#include <cstddef>
#include <deque>
#include <span>
#include <vector>

namespace moex::twime_sbe {

struct TwimeFrameView {
    std::vector<std::byte> bytes;
};

struct TwimeFrameFeedResult {
    TwimeDecodeError error{TwimeDecodeError::Ok};
    std::size_t frames_ready{0};
    std::size_t buffered_bytes{0};
};

class TwimeFrameAssembler {
  public:
    explicit TwimeFrameAssembler(std::size_t max_frame_size);

    [[nodiscard]] TwimeFrameFeedResult feed(std::span<const std::byte> bytes);
    [[nodiscard]] bool has_frame() const noexcept;
    [[nodiscard]] TwimeFrameView pop_frame();
    void reset();

  private:
    std::size_t max_frame_size_;
    std::vector<std::byte> buffer_;
    std::deque<std::vector<std::byte>> ready_frames_;
};

} // namespace moex::twime_sbe
