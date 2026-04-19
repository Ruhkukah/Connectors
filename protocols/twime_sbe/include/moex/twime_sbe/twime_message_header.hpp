#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace moex::twime_sbe {

enum class TwimeDecodeError {
    Ok,
    NeedMoreData,
    BufferTooSmall,
    InvalidHeaderSize,
    InvalidBlockLength,
    UnsupportedSchemaId,
    UnsupportedVersion,
    UnknownTemplateId,
    InvalidEnumValue,
    InvalidSetValue,
    InvalidStringEncoding,
    InvalidFieldValue,
    TrailingBytes,
    InternalError,
};

#pragma pack(push, 1)
struct TwimeMessageHeader {
    std::uint16_t block_length;
    std::uint16_t template_id;
    std::uint16_t schema_id;
    std::uint16_t version;
};
#pragma pack(pop)

static_assert(sizeof(TwimeMessageHeader) == 8);
static_assert(alignof(TwimeMessageHeader) == 1);

constexpr std::size_t kTwimeMessageHeaderSize = sizeof(TwimeMessageHeader);

inline constexpr std::uint16_t load_u16_le(std::span<const std::byte, 2> bytes) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[0])) |
           (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[1])) << 8U);
}

inline constexpr void store_u16_le(std::uint16_t value, std::span<std::byte, 2> bytes) noexcept {
    bytes[0] = static_cast<std::byte>(value & 0xFFU);
    bytes[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

inline constexpr TwimeDecodeError decode_twime_message_header(std::span<const std::byte> bytes,
                                                              TwimeMessageHeader& header) noexcept {
    if (bytes.size() != kTwimeMessageHeaderSize) {
        return TwimeDecodeError::InvalidHeaderSize;
    }

    header.block_length = load_u16_le(bytes.first<2>());
    header.template_id = load_u16_le(bytes.subspan<2, 2>());
    header.schema_id = load_u16_le(bytes.subspan<4, 2>());
    header.version = load_u16_le(bytes.subspan<6, 2>());
    return TwimeDecodeError::Ok;
}

inline constexpr std::array<std::byte, kTwimeMessageHeaderSize>
encode_twime_message_header(const TwimeMessageHeader& header) noexcept {
    std::array<std::byte, kTwimeMessageHeaderSize> bytes{};
    store_u16_le(header.block_length, std::span<std::byte, 2>(bytes.data(), 2));
    store_u16_le(header.template_id, std::span<std::byte, 2>(bytes.data() + 2, 2));
    store_u16_le(header.schema_id, std::span<std::byte, 2>(bytes.data() + 4, 2));
    store_u16_le(header.version, std::span<std::byte, 2>(bytes.data() + 6, 2));
    return bytes;
}

} // namespace moex::twime_sbe
