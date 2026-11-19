#pragma once

// Wire additions for the MOEX ASTS (securities and FX) FAST gate on top of the
// shared stop-bit primitives. Unlike Spectra, ASTS templates use the copy,
// default and increment operators, so messages and sequence entries carry
// presence maps. The gate resets its dictionary after every UDP packet and
// sends one message per packet, so operator state never outlives a message.

#include "moex/fast/fast_wire.hpp"

#include <cstdint>

namespace moex::fast::asts {

// Presence map, left-aligned: the next bit to consume is the top bit. Bits past
// the encoded length read as zero, as FAST requires.
[[nodiscard]] inline std::uint64_t read_pmap(Reader& r) noexcept {
    std::uint64_t bits = 0;
    int shift = 57;
    while (r.p < r.end) {
        const std::uint8_t byte = *r.p++;
        if (shift >= 0)
            bits |= std::uint64_t{byte & 0x7Fu} << shift;
        shift -= 7;
        if (byte & 0x80u)
            return bits;
    }
    return r.fail();
}

[[nodiscard]] inline bool take(std::uint64_t& pmap) noexcept {
    const bool bit = pmap >> 63;
    pmap <<= 1;
    return bit;
}

// Consumes the message presence map and template id; returns 0 on malformed
// input. The remaining presence bits are left in pmap for the message decoder.
[[nodiscard]] inline std::uint32_t read_header(Reader& r, std::uint64_t& pmap) noexcept {
    pmap = read_pmap(r);
    if (!r.ok || !take(pmap)) {
        r.fail();
        return 0;
    }
    const std::uint32_t id = read_u32(r);
    return r.ok ? id : 0;
}

// Value of a field whose default comes from the template rather than the stream.
template <std::size_t N> [[nodiscard]] constexpr Ascii literal(const std::uint8_t (&text)[N]) noexcept {
    return {text, static_cast<std::uint32_t>(N)};
}

} // namespace moex::fast::asts
