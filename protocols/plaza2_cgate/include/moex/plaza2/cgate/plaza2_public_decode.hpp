#pragma once

#include "plaza2_public_wire.hpp"

#include <bit>
#include <cassert>
#include <type_traits>
#include <cstring>
#include <optional>

namespace moex::plaza2::public_wire {

static_assert(std::endian::native == std::endian::little);

// Preserve the original BCD alongside this exact convenience conversion.
// d16.5: scale/precision, base-100 digits, sign in the first digit, final half digit.
// Qualified against CGate 9.9 cg_bcd_get in a network-disabled SDK run.
inline std::optional<std::int64_t> decimal_scaled(const Bcd16_5& bytes) noexcept {
    if (bytes[0] != 5 || bytes[1] != 16) {
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
    return negative ? -value : value;
}

// Precondition: validate the complete wire record once before loading any fields.
template <typename T> inline T load(std::span<const std::byte> bytes, std::size_t offset = 0) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    assert(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset);
    T result;
    std::memcpy(&result, bytes.data() + offset, sizeof(T));
    return result;
}

inline const Table* table(generated::TableCode code) noexcept {
    for (const auto& t : kTables) {
        if (t.code == code) {
            return &t;
        }
    }
    return nullptr;
}

enum class DecodeResult : std::uint8_t { Ok, Size, NullMap, Decimal };

inline DecodeResult validate(const Table& table, std::span<const std::byte> bytes,
                             std::span<const std::uint8_t> nulls) noexcept {
    if (bytes.size() != table.size) {
        return DecodeResult::Size;
    }
    if (!nulls.empty() && nulls.size() != table.fields.size()) {
        return DecodeResult::NullMap;
    }
    for (std::size_t i = 0; i < table.fields.size(); ++i) {
        if (!nulls.empty() && nulls[i] != 0) {
            if (nulls[i] != 1 || i < 3) {
                return DecodeResult::NullMap;
            }
            continue;
        }
        const auto& field = table.fields[i];
        if (field.type == "d16.5" && !decimal_scaled(load<Bcd16_5>(bytes, field.offset))) {
            return DecodeResult::Decimal;
        }
    }
    return DecodeResult::Ok;
}

} // namespace moex::plaza2::public_wire
