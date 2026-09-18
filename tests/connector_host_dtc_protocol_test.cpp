#include "moex/connector_host/dtc_market_data.hpp"

#include <cstdlib>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::vector<std::uint8_t> frame(std::uint16_t type, std::initializer_list<std::uint8_t> payload) {
    const auto size = static_cast<std::uint16_t>(4 + payload.size());
    std::vector<std::uint8_t> out{static_cast<std::uint8_t>(size & 0xffU), static_cast<std::uint8_t>(size >> 8U),
                                  static_cast<std::uint8_t>(type & 0xffU), static_cast<std::uint8_t>(type >> 8U)};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

} // namespace

int main() {
    using moex::connector_host::dtc::DtcFrameDecoder;
    using moex::connector_host::dtc::kDtcFrameHeaderSize;

    DtcFrameDecoder decoder;
    std::vector<moex::connector_host::dtc::DtcFrame> frames;
    std::string error;
    const auto first = frame(6, {1, 2, 3});
    const auto second = frame(145, {9});
    std::vector<std::uint8_t> combined = first;
    combined.insert(combined.end(), second.begin(), second.end());

    const auto partial_body = kDtcFrameHeaderSize + 1;
    require(decoder.append(std::span<const std::uint8_t>(combined.data(), partial_body), frames, error),
            "fragment header and partial body rejected");
    require(frames.empty(), "partial frame emitted");
    require(
        decoder.append(std::span<const std::uint8_t>(combined.data() + partial_body, combined.size() - partial_body),
                       frames, error),
        "remaining frame and coalesced frame rejected");
    require(frames.size() == 2, "coalesced frames were not both emitted");
    require(frames[0].message_type == 6 && frames[0].payload == std::vector<std::uint8_t>({1, 2, 3}),
            "first frame decoded incorrectly");
    require(frames[1].message_type == 145 && frames[1].payload == std::vector<std::uint8_t>({9}),
            "second frame decoded incorrectly");
    require(decoder.buffered_bytes() == 0, "decoder retained completed bytes");

    const std::vector<std::uint8_t> invalid{3, 0, 1, 0};
    require(!decoder.append(invalid, frames, error), "short frame length accepted");
    require(error.find("invalid DTC frame length 3") != std::string::npos, "invalid frame error was not causal");
    require(decoder.buffered_bytes() == 0, "invalid frame did not fence decoder");
    require(!decoder.append(std::span<const std::uint8_t>(first.data(), first.size()), frames, error),
            "decoder silently resynchronized after an invalid frame");
    decoder.reset();

    // A single socket read may contain many valid frames and may exceed the
    // uint16 frame-size limit in aggregate. Only an incomplete individual
    // frame is retained in the bounded receive buffer.
    frames.clear();
    std::vector<std::uint8_t> large_burst;
    for (int index = 0; index < 9000; ++index) {
        const auto item = frame(static_cast<std::uint16_t>(100 + (index % 20)), {1, 2, 3, 4, 5, 6, 7, 8});
        large_burst.insert(large_burst.end(), item.begin(), item.end());
    }
    require(large_burst.size() > 65535, "coalesced test burst must exceed the maximum individual frame size");
    require(decoder.append(large_burst, frames, error), "large coalesced burst rejected");
    require(frames.size() == 9000 && decoder.buffered_bytes() == 0, "large coalesced burst was not fully decoded");

    // Exercise every header/body split point with a fresh decoder state.
    for (std::size_t split = 1; split < first.size(); ++split) {
        decoder.reset();
        frames.clear();
        require(decoder.append(std::span<const std::uint8_t>(first.data(), split), frames, error),
                "fragment prefix rejected");
        require(frames.empty(), "fragment prefix emitted a frame");
        require(
            decoder.append(std::span<const std::uint8_t>(first.data() + split, first.size() - split), frames, error),
            "fragment suffix rejected");
        require(frames.size() == 1 && frames.front().message_type == 6, "fragmented frame mismatch");
    }

    decoder.reset();
    frames.clear();
    require(decoder.append(std::span<const std::uint8_t>(first.data(), first.size() - 1), frames, error),
            "truncated frame prefix rejected before disconnect");
    require(!decoder.finish(error) && error.find("truncated") != std::string::npos,
            "truncated frame disconnect was not reported");
    require(!decoder.append(std::span<const std::uint8_t>(first.data(), first.size()), frames, error),
            "decoder silently accepted data after truncated disconnect");
    decoder.reset();

    const moex::connector_host::dtc::DtcReadOnlyCapabilities capabilities{};
    require(!capabilities.order_entry && !capabilities.accounts && !capabilities.positions,
            "read-only DTC capabilities unexpectedly advertise execution");
    return 0;
}
