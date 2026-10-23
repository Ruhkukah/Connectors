#pragma once

#include "plaza2_fixed_point.hpp"
#include <array>
#include <cstdint>
#include <span>

#include <bit>
#include <cassert>
#include <type_traits>
#include <cstring>
#include <optional>

namespace moex::plaza2::public_wire {

static_assert(std::endian::native == std::endian::little);

#pragma pack(push, 4)
struct Time {
    std::uint16_t year;
    std::uint8_t month, day, hour, minute, second;
    std::uint16_t msec;
};
#pragma pack(pop)
static_assert(sizeof(Time) == 10 && offsetof(Time, msec) == 8);
using Bcd16_5 = std::array<std::uint8_t, 11>;

struct ExactDecimal {
    std::int64_t mantissa{0};
    std::int32_t scale{0};
};

// Preserve the original BCD alongside this exact convenience conversion.
// d16.5: scale/precision, base-100 digits, sign in the first digit, final half digit.
// Qualified against CGate 9.9 cg_bcd_get in a network-disabled SDK run.
inline std::optional<ExactDecimal> decimal_value(const Bcd16_5& bytes) noexcept {
    if (bytes[0] != cgate::kPlaza2D16_5FractionalDigits || bytes[1] != cgate::kPlaza2D16_5DecimalPrecision) {
        return std::nullopt;
    }
    const bool negative = (bytes[2] & 0x80U) != 0;
    std::int64_t value = bytes[2] & 0x7FU;
    if (value > 9) {
        return std::nullopt;
    }
    for (std::size_t i = 3; i < bytes.size(); ++i) {
        const auto digit = bytes[i] == 0x80U ? 0U : bytes[i];
        if (digit > 99) {
            return std::nullopt;
        }
        value = value * 100 + digit;
    }
    if (value % 10 != 0) {
        return std::nullopt;
    }
    value /= 10;
    return ExactDecimal{.mantissa = negative ? -value : value,
                        .scale = static_cast<std::int32_t>(cgate::kPlaza2D16_5FractionalDigits)};
}

inline std::optional<std::int64_t> decimal_scaled(const Bcd16_5& bytes) noexcept {
    const auto value = decimal_value(bytes);
    return value.has_value() ? std::optional<std::int64_t>{value->mantissa} : std::nullopt;
}

// Precondition: validate the complete wire record once before loading any fields.
template <typename T> inline T load(std::span<const std::byte> bytes, std::size_t offset = 0) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    assert(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset);
    T result;
    std::memcpy(&result, bytes.data() + offset, sizeof(T));
    return result;
}

} // namespace moex::plaza2::public_wire
