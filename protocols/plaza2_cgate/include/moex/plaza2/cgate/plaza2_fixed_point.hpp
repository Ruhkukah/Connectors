#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace moex::plaza2::cgate {

// Parse an exact decimal into signed integer units. The scale is explicit at
// every call site because each CGate decimal descriptor is its own contract.
//
// The parser accepts an optional leading sign only when allow_sign is true,
// requires digits on both sides of a decimal point when the point is present,
// rejects excess precision and rejects all arithmetic overflow. A non-zero
// decimal_precision also enforces the descriptor's total precision after
// padding to the requested scale. It never rounds, truncates, skips
// characters, or passes through a floating-point conversion.
[[nodiscard]] inline std::optional<std::int64_t> parse_fixed_point(std::string_view text, unsigned fractional_digits,
                                                                   bool allow_sign,
                                                                   unsigned decimal_precision = 0) noexcept {
    if (text.empty() || fractional_digits > 18 || (decimal_precision != 0 && decimal_precision < fractional_digits)) {
        return std::nullopt;
    }

    bool negative = false;
    if (text.front() == '+' || text.front() == '-') {
        if (!allow_sign) {
            return std::nullopt;
        }
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
                       (negative ? std::uint64_t{1} : std::uint64_t{0});
    std::uint64_t magnitude = 0;
    unsigned fractional = 0;
    unsigned whole_digits = 0;
    bool point = false;
    bool whole_digit = false;
    bool fraction_digit = false;

    for (const char character : text) {
        if (character == '.') {
            if (point || !whole_digit) {
                return std::nullopt;
            }
            point = true;
            continue;
        }
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        if (point) {
            if (fractional++ >= fractional_digits) {
                return std::nullopt;
            }
            fraction_digit = true;
        } else {
            whole_digit = true;
            ++whole_digits;
        }

        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (magnitude > (limit - digit) / 10U) {
            return std::nullopt;
        }
        magnitude = magnitude * 10U + digit;
    }

    if (!whole_digit || (point && !fraction_digit)) {
        return std::nullopt;
    }
    if (decimal_precision != 0 && whole_digits > decimal_precision - fractional_digits) {
        return std::nullopt;
    }

    for (unsigned index = fractional; index < fractional_digits; ++index) {
        if (magnitude > limit / 10U) {
            return std::nullopt;
        }
        magnitude *= 10U;
    }

    if (negative && magnitude == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if (negative) {
        return -static_cast<std::int64_t>(magnitude);
    }
    return static_cast<std::int64_t>(magnitude);
}

// Generated SPECTRA metadata: d16.5 means precision 16 and scale 5.
inline constexpr unsigned kPlaza2D16_5DecimalPrecision = 16;
inline constexpr unsigned kPlaza2D16_5FractionalDigits = 5;
inline constexpr std::int64_t kPlaza2D16_5PriceScale = 100'000;

// Compatibility aliases retained for callers that name their source domain.
// They must remain exact aliases of the generated d16.5 economic contract.
inline constexpr unsigned kPlaza2SessionFractionalDigits = kPlaza2D16_5FractionalDigits;
inline constexpr unsigned kPlaza2Aggr20FractionalDigits = kPlaza2D16_5FractionalDigits;
inline constexpr std::int64_t kPlaza2SessionPriceScale = kPlaza2D16_5PriceScale;
inline constexpr std::int64_t kPlaza2Aggr20PriceScale = kPlaza2D16_5PriceScale;

static_assert(kPlaza2D16_5DecimalPrecision == 16);
static_assert(kPlaza2D16_5FractionalDigits == 5);
static_assert(kPlaza2D16_5PriceScale == 100'000);

} // namespace moex::plaza2::cgate
