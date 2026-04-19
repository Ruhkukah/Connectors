#pragma once

#include "moex/twime_sbe/twime_message_header.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace moex::twime_sbe {

struct TwimeFieldMetadata;
struct TwimeMessageMetadata;

struct TwimeDecimal5 {
    std::int64_t mantissa{0};
    static constexpr std::int8_t exponent = -5;
};

template <std::size_t Length> struct TwimeFixedString {
    std::array<char, Length> bytes{};

    void assign(std::string_view value) noexcept {
        std::fill(bytes.begin(), bytes.end(), '\0');
        const auto count = std::min(value.size(), bytes.size());
        std::copy_n(value.begin(), count, bytes.begin());
    }

    [[nodiscard]] std::string_view trimmed_view() const noexcept {
        std::size_t count = bytes.size();
        while (count > 0 && (bytes[count - 1] == '\0' || bytes[count - 1] == ' ')) {
            --count;
        }
        return std::string_view(bytes.data(), count);
    }
};

using String7 = TwimeFixedString<7>;
using String20 = TwimeFixedString<20>;
using String25 = TwimeFixedString<25>;

constexpr std::uint64_t kTwimeTimestampNull = std::numeric_limits<std::uint64_t>::max();

enum class TwimeValueKind {
    Signed,
    Unsigned,
    Decimal5,
    String,
    EnumName,
    SetName,
    TimeStamp,
    DeltaMillisecs,
};

struct TwimeFieldValue {
    TwimeValueKind kind{TwimeValueKind::Unsigned};
    std::int64_t signed_value{0};
    std::uint64_t unsigned_value{0};
    TwimeDecimal5 decimal5{};
    std::array<char, 25> string_bytes{};
    std::uint8_t string_length{0};

    static TwimeFieldValue signed_integer(std::int64_t value) noexcept;
    static TwimeFieldValue unsigned_integer(std::uint64_t value) noexcept;
    static TwimeFieldValue decimal(std::int64_t mantissa) noexcept;
    static TwimeFieldValue timestamp(std::uint64_t value) noexcept;
    static TwimeFieldValue delta_millisecs(std::uint32_t value) noexcept;
    static TwimeFieldValue string(std::string_view value) noexcept;
    static TwimeFieldValue enum_name(std::string_view value) noexcept;
    static TwimeFieldValue set_name(std::string_view value) noexcept;

    [[nodiscard]] std::string_view string_view() const noexcept {
        return std::string_view(string_bytes.data(), string_length);
    }
};

struct TwimeFieldInput {
    std::string name;
    TwimeFieldValue value;
};

struct TwimeEncodeRequest {
    std::string message_name;
    std::uint16_t template_id{0};
    std::vector<TwimeFieldInput> fields;
};

struct DecodedTwimeField {
    const TwimeFieldMetadata* metadata{nullptr};
    TwimeFieldValue value{};
};

struct DecodedTwimeMessage {
    TwimeMessageHeader header{};
    const TwimeMessageMetadata* metadata{nullptr};
    std::vector<DecodedTwimeField> fields;
};

inline TwimeFieldValue TwimeFieldValue::signed_integer(std::int64_t value) noexcept {
    TwimeFieldValue out;
    out.kind = TwimeValueKind::Signed;
    out.signed_value = value;
    return out;
}

inline TwimeFieldValue TwimeFieldValue::unsigned_integer(std::uint64_t value) noexcept {
    TwimeFieldValue out;
    out.kind = TwimeValueKind::Unsigned;
    out.unsigned_value = value;
    return out;
}

inline TwimeFieldValue TwimeFieldValue::decimal(std::int64_t mantissa) noexcept {
    TwimeFieldValue out;
    out.kind = TwimeValueKind::Decimal5;
    out.decimal5.mantissa = mantissa;
    return out;
}

inline TwimeFieldValue TwimeFieldValue::timestamp(std::uint64_t value) noexcept {
    TwimeFieldValue out;
    out.kind = TwimeValueKind::TimeStamp;
    out.unsigned_value = value;
    return out;
}

inline TwimeFieldValue TwimeFieldValue::delta_millisecs(std::uint32_t value) noexcept {
    TwimeFieldValue out;
    out.kind = TwimeValueKind::DeltaMillisecs;
    out.unsigned_value = value;
    return out;
}

inline TwimeFieldValue TwimeFieldValue::string(std::string_view value) noexcept {
    TwimeFieldValue out;
    out.kind = TwimeValueKind::String;
    out.string_length = static_cast<std::uint8_t>(std::min<std::size_t>(value.size(), out.string_bytes.size()));
    std::fill(out.string_bytes.begin(), out.string_bytes.end(), '\0');
    std::copy_n(value.begin(), out.string_length, out.string_bytes.begin());
    return out;
}

inline TwimeFieldValue TwimeFieldValue::enum_name(std::string_view value) noexcept {
    TwimeFieldValue out = string(value);
    out.kind = TwimeValueKind::EnumName;
    return out;
}

inline TwimeFieldValue TwimeFieldValue::set_name(std::string_view value) noexcept {
    TwimeFieldValue out = string(value);
    out.kind = TwimeValueKind::SetName;
    return out;
}

} // namespace moex::twime_sbe
