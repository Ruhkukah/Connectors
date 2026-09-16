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
    std::vector<std::uint8_t> out{static_cast<std::uint8_t>(size & 0xffU),
                                  static_cast<std::uint8_t>(size >> 8U),
                                  static_cast<std::uint8_t>(type & 0xffU),
                                  static_cast<std::uint8_t>(type >> 8U)};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

} // namespace

int main() {
    using moex::connector_host::dtc::DtcFrameDecoder;

    DtcFrameDecoder decoder;
    std::vector<moex::connector_host::dtc::DtcFrame> frames;
    std::string error;
    const auto first = frame(6, {1, 2, 3});
    const auto second = frame(145, {9});
    std::vector<std::uint8_t> combined = first;
    combined.insert(combined.end(), second.begin(), second.end());

    require(decoder.append(std::span<const std::uint8_t>(combined.data(), 2), frames, error),
            "fragment header rejected");
    require(frames.empty(), "partial frame emitted");
    require(decoder.append(std::span<const std::uint8_t>(combined.data() + 2, combined.size() - 2),
                           frames, error),
            "coalesced frames rejected");
    require(frames.size() == 2, "coalesced frames were not both emitted");
    require(frames[0].message_type == 6 && frames[0].payload == std::vector<std::uint8_t>({1, 2, 3}),
            "first frame decoded incorrectly");
    require(frames[1].message_type == 145 && frames[1].payload == std::vector<std::uint8_t>({9}),
            "second frame decoded incorrectly");
    require(decoder.buffered_bytes() == 0, "decoder retained completed bytes");

    const std::vector<std::uint8_t> invalid{3, 0, 1, 0};
    require(!decoder.append(invalid, frames, error), "short frame length accepted");
    require(error.find("invalid DTC frame length 3") != std::string::npos,
            "invalid frame error was not causal");
    require(decoder.buffered_bytes() == 0, "invalid frame did not fence decoder");

    const moex::connector_host::dtc::DtcReadOnlyCapabilities capabilities{};
    require(!capabilities.order_entry && !capabilities.accounts && !capabilities.positions,
            "read-only DTC capabilities unexpectedly advertise execution");
    return 0;
}
