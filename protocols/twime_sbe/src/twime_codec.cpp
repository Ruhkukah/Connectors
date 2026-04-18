#include "moex/twime_sbe/twime_codec.hpp"

#include "twime_generated_metadata.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <sstream>

namespace moex::twime_sbe {

namespace {

template <typename T>
T load_le(std::span<const std::byte> bytes) {
    std::array<std::byte, sizeof(T)> local{};
    std::copy_n(bytes.begin(), sizeof(T), local.begin());
    T value{};
    std::memcpy(&value, local.data(), sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        return value;
    }
    if constexpr (sizeof(T) == sizeof(std::uint16_t)) {
        const auto raw = static_cast<std::uint16_t>(value);
        return static_cast<T>((raw >> 8U) | (raw << 8U));
    }
    if constexpr (sizeof(T) == sizeof(std::uint32_t)) {
        const auto raw = static_cast<std::uint32_t>(value);
        return static_cast<T>(
            ((raw & 0x000000FFu) << 24U) |
            ((raw & 0x0000FF00u) << 8U) |
            ((raw & 0x00FF0000u) >> 8U) |
            ((raw & 0xFF000000u) >> 24U));
    }
    if constexpr (sizeof(T) == sizeof(std::uint64_t)) {
        const auto raw = static_cast<std::uint64_t>(value);
        return static_cast<T>(
            ((raw & 0x00000000000000FFULL) << 56U) |
            ((raw & 0x000000000000FF00ULL) << 40U) |
            ((raw & 0x0000000000FF0000ULL) << 24U) |
            ((raw & 0x00000000FF000000ULL) << 8U) |
            ((raw & 0x000000FF00000000ULL) >> 8U) |
            ((raw & 0x0000FF0000000000ULL) >> 24U) |
            ((raw & 0x00FF000000000000ULL) >> 40U) |
            ((raw & 0xFF00000000000000ULL) >> 56U));
    }
    return value;
}

template <typename T>
void store_le(T value, std::span<std::byte> bytes) {
    T local = value;
    if constexpr (std::endian::native != std::endian::little) {
        if constexpr (sizeof(T) == sizeof(std::uint16_t)) {
            const auto raw = static_cast<std::uint16_t>(value);
            local = static_cast<T>((raw >> 8U) | (raw << 8U));
        } else if constexpr (sizeof(T) == sizeof(std::uint32_t)) {
            const auto raw = static_cast<std::uint32_t>(value);
            local = static_cast<T>(
                ((raw & 0x000000FFu) << 24U) |
                ((raw & 0x0000FF00u) << 8U) |
                ((raw & 0x00FF0000u) >> 8U) |
                ((raw & 0xFF000000u) >> 24U));
        } else if constexpr (sizeof(T) == sizeof(std::uint64_t)) {
            const auto raw = static_cast<std::uint64_t>(value);
            local = static_cast<T>(
                ((raw & 0x00000000000000FFULL) << 56U) |
                ((raw & 0x000000000000FF00ULL) << 40U) |
                ((raw & 0x0000000000FF0000ULL) << 24U) |
                ((raw & 0x00000000FF000000ULL) << 8U) |
                ((raw & 0x000000FF00000000ULL) >> 8U) |
                ((raw & 0x0000FF0000000000ULL) >> 24U) |
                ((raw & 0x00FF000000000000ULL) >> 40U) |
                ((raw & 0xFF00000000000000ULL) >> 56U));
        }
    }
    std::memcpy(bytes.data(), &local, sizeof(T));
}

std::string trim_copy(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

const TwimeFieldInput* find_input_field(const TwimeEncodeRequest& request, std::string_view name) {
    for (const auto& field : request.fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

const TwimeEnumValueMetadata* find_enum_value_by_name(const TwimeEnumMetadata& metadata, std::string_view name) {
    for (std::size_t index = 0; index < metadata.value_count; ++index) {
        if (metadata.values[index].name == name) {
            return metadata.values + index;
        }
    }
    return nullptr;
}

const TwimeEnumValueMetadata* find_enum_value_by_wire(const TwimeEnumMetadata& metadata, std::int64_t wire_value) {
    for (std::size_t index = 0; index < metadata.value_count; ++index) {
        if (metadata.values[index].wire_value == wire_value) {
            return metadata.values + index;
        }
    }
    return nullptr;
}

std::uint64_t parse_set_names(const TwimeSetMetadata& metadata, std::string_view names) {
    if (names.empty()) {
        return 0;
    }
    std::uint64_t mask = 0;
    std::size_t start = 0;
    while (start <= names.size()) {
        const auto next = names.find('|', start);
        const auto token = trim_copy(names.substr(start, next == std::string_view::npos ? names.size() - start : next - start));
        for (std::size_t index = 0; index < metadata.choice_count; ++index) {
            if (metadata.choices[index].name == token) {
                mask |= (static_cast<std::uint64_t>(1) << metadata.choices[index].bit_index);
                break;
            }
        }
        if (next == std::string_view::npos) {
            break;
        }
        start = next + 1;
    }
    return mask;
}

TwimeFieldValue decode_field_value(
    const TwimeFieldMetadata& field,
    std::span<const std::byte> bytes,
    TwimeDecodeError& error) {
    const auto& type = *field.type;
    error = TwimeDecodeError::Ok;

    switch (type.kind) {
        case TwimeFieldKind::Primitive: {
            switch (type.primitive_type) {
                case TwimePrimitiveType::Int8:
                    return TwimeFieldValue::signed_integer(load_le<std::int8_t>(bytes));
                case TwimePrimitiveType::Int16:
                    return TwimeFieldValue::signed_integer(load_le<std::int16_t>(bytes));
                case TwimePrimitiveType::Int32:
                    return TwimeFieldValue::signed_integer(load_le<std::int32_t>(bytes));
                case TwimePrimitiveType::Int64:
                    return TwimeFieldValue::signed_integer(load_le<std::int64_t>(bytes));
                case TwimePrimitiveType::UInt8:
                    return TwimeFieldValue::unsigned_integer(load_le<std::uint8_t>(bytes));
                case TwimePrimitiveType::UInt16:
                    return TwimeFieldValue::unsigned_integer(load_le<std::uint16_t>(bytes));
                case TwimePrimitiveType::UInt32:
                    return TwimeFieldValue::unsigned_integer(load_le<std::uint32_t>(bytes));
                case TwimePrimitiveType::UInt64:
                    return TwimeFieldValue::unsigned_integer(load_le<std::uint64_t>(bytes));
                default:
                    error = TwimeDecodeError::InvalidFieldValue;
                    return {};
            }
        }
        case TwimeFieldKind::DeltaMillisecs:
            return TwimeFieldValue::delta_millisecs(load_le<std::uint32_t>(bytes));
        case TwimeFieldKind::TimeStamp:
            return TwimeFieldValue::timestamp(load_le<std::uint64_t>(bytes));
        case TwimeFieldKind::Decimal5:
            return TwimeFieldValue::decimal(load_le<std::int64_t>(bytes));
        case TwimeFieldKind::String: {
            TwimeFieldValue value = TwimeFieldValue::string({});
            value.string_length = static_cast<std::uint8_t>(std::min<std::size_t>(type.fixed_length, value.string_bytes.size()));
            for (std::size_t index = 0; index < value.string_length; ++index) {
                value.string_bytes[index] = static_cast<char>(std::to_integer<unsigned char>(bytes[index]));
            }
            while (value.string_length > 0 &&
                   (value.string_bytes[value.string_length - 1] == '\0' ||
                    value.string_bytes[value.string_length - 1] == ' ')) {
                --value.string_length;
            }
            return value;
        }
        case TwimeFieldKind::Enum: {
            std::int64_t wire_value = 0;
            switch (type.primitive_type) {
                case TwimePrimitiveType::Char:
                    wire_value = load_le<char>(bytes);
                    break;
                case TwimePrimitiveType::UInt8:
                    wire_value = load_le<std::uint8_t>(bytes);
                    break;
                default:
                    error = TwimeDecodeError::InvalidFieldValue;
                    return {};
            }
            if (type.enum_metadata == nullptr || find_enum_value_by_wire(*type.enum_metadata, wire_value) == nullptr) {
                error = TwimeDecodeError::InvalidEnumValue;
                return {};
            }
            return TwimeFieldValue::unsigned_integer(static_cast<std::uint64_t>(wire_value));
        }
        case TwimeFieldKind::Set: {
            std::uint64_t wire_value = 0;
            switch (type.primitive_type) {
                case TwimePrimitiveType::UInt8:
                    wire_value = load_le<std::uint8_t>(bytes);
                    break;
                case TwimePrimitiveType::UInt64:
                    wire_value = load_le<std::uint64_t>(bytes);
                    break;
                default:
                    error = TwimeDecodeError::InvalidFieldValue;
                    return {};
            }
            return TwimeFieldValue::unsigned_integer(wire_value);
        }
        case TwimeFieldKind::Composite:
        default:
            error = TwimeDecodeError::InvalidFieldValue;
            return {};
    }
}

TwimeDecodeError encode_field_value(
    const TwimeFieldMetadata& field,
    const TwimeFieldValue& value,
    std::span<std::byte> out_bytes) {
    const auto& type = *field.type;
    if (out_bytes.size() != field.encoded_size) {
        return TwimeDecodeError::BufferTooSmall;
    }

    switch (type.kind) {
        case TwimeFieldKind::Primitive: {
            if (value.kind == TwimeValueKind::Signed) {
                switch (type.primitive_type) {
                    case TwimePrimitiveType::Int8:
                        store_le<std::int8_t>(static_cast<std::int8_t>(value.signed_value), out_bytes);
                        return TwimeDecodeError::Ok;
                    case TwimePrimitiveType::Int16:
                        store_le<std::int16_t>(static_cast<std::int16_t>(value.signed_value), out_bytes);
                        return TwimeDecodeError::Ok;
                    case TwimePrimitiveType::Int32:
                        store_le<std::int32_t>(static_cast<std::int32_t>(value.signed_value), out_bytes);
                        return TwimeDecodeError::Ok;
                    case TwimePrimitiveType::Int64:
                        store_le<std::int64_t>(value.signed_value, out_bytes);
                        return TwimeDecodeError::Ok;
                    default:
                        return TwimeDecodeError::InvalidFieldValue;
                }
            }
            if (value.kind == TwimeValueKind::Unsigned) {
                switch (type.primitive_type) {
                    case TwimePrimitiveType::UInt8:
                        store_le<std::uint8_t>(static_cast<std::uint8_t>(value.unsigned_value), out_bytes);
                        return TwimeDecodeError::Ok;
                    case TwimePrimitiveType::UInt16:
                        store_le<std::uint16_t>(static_cast<std::uint16_t>(value.unsigned_value), out_bytes);
                        return TwimeDecodeError::Ok;
                    case TwimePrimitiveType::UInt32:
                        store_le<std::uint32_t>(static_cast<std::uint32_t>(value.unsigned_value), out_bytes);
                        return TwimeDecodeError::Ok;
                    case TwimePrimitiveType::UInt64:
                        store_le<std::uint64_t>(value.unsigned_value, out_bytes);
                        return TwimeDecodeError::Ok;
                    default:
                        return TwimeDecodeError::InvalidFieldValue;
                }
            }
            return TwimeDecodeError::InvalidFieldValue;
        }
        case TwimeFieldKind::DeltaMillisecs:
            if (value.kind != TwimeValueKind::DeltaMillisecs && value.kind != TwimeValueKind::Unsigned) {
                return TwimeDecodeError::InvalidFieldValue;
            }
            store_le<std::uint32_t>(static_cast<std::uint32_t>(value.unsigned_value), out_bytes);
            return TwimeDecodeError::Ok;
        case TwimeFieldKind::TimeStamp:
            if (value.kind != TwimeValueKind::TimeStamp && value.kind != TwimeValueKind::Unsigned) {
                return TwimeDecodeError::InvalidFieldValue;
            }
            store_le<std::uint64_t>(value.unsigned_value, out_bytes);
            return TwimeDecodeError::Ok;
        case TwimeFieldKind::Decimal5:
            if (value.kind != TwimeValueKind::Decimal5) {
                return TwimeDecodeError::InvalidFieldValue;
            }
            store_le<std::int64_t>(value.decimal5.mantissa, out_bytes);
            return TwimeDecodeError::Ok;
        case TwimeFieldKind::String: {
            if (value.kind != TwimeValueKind::String) {
                return TwimeDecodeError::InvalidStringEncoding;
            }
            std::fill(out_bytes.begin(), out_bytes.end(), std::byte{0});
            for (std::size_t index = 0; index < std::min<std::size_t>(type.fixed_length, value.string_length); ++index) {
                out_bytes[index] = static_cast<std::byte>(value.string_bytes[index]);
            }
            return TwimeDecodeError::Ok;
        }
        case TwimeFieldKind::Enum: {
            std::int64_t wire_value = 0;
            if (value.kind == TwimeValueKind::EnumName || value.kind == TwimeValueKind::String) {
                const auto* enum_value = type.enum_metadata == nullptr
                    ? nullptr
                    : find_enum_value_by_name(*type.enum_metadata, value.string_view());
                if (enum_value == nullptr) {
                    return TwimeDecodeError::InvalidEnumValue;
                }
                wire_value = enum_value->wire_value;
            } else if (value.kind == TwimeValueKind::Unsigned) {
                wire_value = static_cast<std::int64_t>(value.unsigned_value);
                if (type.enum_metadata != nullptr && find_enum_value_by_wire(*type.enum_metadata, wire_value) == nullptr) {
                    return TwimeDecodeError::InvalidEnumValue;
                }
            } else if (value.kind == TwimeValueKind::Signed) {
                wire_value = value.signed_value;
            } else {
                return TwimeDecodeError::InvalidFieldValue;
            }

            switch (type.primitive_type) {
                case TwimePrimitiveType::Char:
                    store_le<char>(static_cast<char>(wire_value), out_bytes);
                    return TwimeDecodeError::Ok;
                case TwimePrimitiveType::UInt8:
                    store_le<std::uint8_t>(static_cast<std::uint8_t>(wire_value), out_bytes);
                    return TwimeDecodeError::Ok;
                default:
                    return TwimeDecodeError::InvalidFieldValue;
            }
        }
        case TwimeFieldKind::Set: {
            std::uint64_t mask = 0;
            if (value.kind == TwimeValueKind::SetName || value.kind == TwimeValueKind::String) {
                if (type.set_metadata == nullptr) {
                    return TwimeDecodeError::InvalidFieldValue;
                }
                mask = parse_set_names(*type.set_metadata, value.string_view());
            } else if (value.kind == TwimeValueKind::Unsigned) {
                mask = value.unsigned_value;
            } else {
                return TwimeDecodeError::InvalidFieldValue;
            }
            switch (type.primitive_type) {
                case TwimePrimitiveType::UInt8:
                    store_le<std::uint8_t>(static_cast<std::uint8_t>(mask), out_bytes);
                    return TwimeDecodeError::Ok;
                case TwimePrimitiveType::UInt64:
                    store_le<std::uint64_t>(mask, out_bytes);
                    return TwimeDecodeError::Ok;
                default:
                    return TwimeDecodeError::InvalidFieldValue;
            }
        }
        case TwimeFieldKind::Composite:
        default:
            return TwimeDecodeError::InvalidFieldValue;
    }
}

}  // namespace

TwimeDecodeError TwimeCodec::encode_message(
    const TwimeEncodeRequest& request,
    std::vector<std::byte>& out_bytes) const {
    const TwimeMessageMetadata* metadata = nullptr;
    if (!request.message_name.empty()) {
        metadata = TwimeSchemaView::find_message_by_name(request.message_name);
    }
    if (metadata == nullptr && request.template_id != 0) {
        metadata = TwimeSchemaView::find_message_by_template_id(request.template_id);
    }
    if (metadata == nullptr) {
        return TwimeDecodeError::UnknownTemplateId;
    }

    out_bytes.assign(kTwimeMessageHeaderSize + metadata->block_length, std::byte{0});
    const TwimeMessageHeader header{
        static_cast<std::uint16_t>(metadata->block_length),
        metadata->template_id,
        TwimeSchemaView::info().schema_id,
        TwimeSchemaView::info().schema_version,
    };
    const auto header_bytes = encode_twime_message_header(header);
    std::copy(header_bytes.begin(), header_bytes.end(), out_bytes.begin());

    for (std::size_t index = 0; index < metadata->field_count; ++index) {
        const auto& field = metadata->fields[index];
        const auto* input = find_input_field(request, field.name);
        if (input == nullptr) {
            return TwimeDecodeError::InvalidFieldValue;
        }
        auto result = encode_field_value(
            field,
            input->value,
            std::span<std::byte>(out_bytes.data() + kTwimeMessageHeaderSize + field.offset, field.encoded_size));
        if (result != TwimeDecodeError::Ok) {
            return result;
        }
    }

    return TwimeDecodeError::Ok;
}

TwimeDecodeError TwimeCodec::decode_message(
    std::span<const std::byte> bytes,
    DecodedTwimeMessage& out_message) const {
    if (bytes.size() < kTwimeMessageHeaderSize) {
        return TwimeDecodeError::NeedMoreData;
    }

    TwimeMessageHeader header{};
    auto header_error = decode_twime_message_header(bytes.first(kTwimeMessageHeaderSize), header);
    if (header_error != TwimeDecodeError::Ok) {
        return header_error;
    }

    if (header.schema_id != TwimeSchemaView::info().schema_id) {
        return TwimeDecodeError::UnsupportedSchemaId;
    }
    if (header.version != TwimeSchemaView::info().schema_version) {
        return TwimeDecodeError::UnsupportedVersion;
    }

    const auto* metadata = TwimeSchemaView::find_message_by_template_id(header.template_id);
    if (metadata == nullptr) {
        return TwimeDecodeError::UnknownTemplateId;
    }
    if (header.block_length != metadata->block_length) {
        return TwimeDecodeError::InvalidBlockLength;
    }

    const auto expected_size = kTwimeMessageHeaderSize + metadata->block_length;
    if (bytes.size() < expected_size) {
        return TwimeDecodeError::NeedMoreData;
    }
    if (bytes.size() > expected_size) {
        return TwimeDecodeError::TrailingBytes;
    }

    out_message.header = header;
    out_message.metadata = metadata;
    out_message.fields.clear();
    out_message.fields.reserve(metadata->field_count);

    for (std::size_t index = 0; index < metadata->field_count; ++index) {
        const auto& field = metadata->fields[index];
        const auto field_bytes = bytes.subspan(kTwimeMessageHeaderSize + field.offset, field.encoded_size);
        TwimeDecodeError field_error = TwimeDecodeError::Ok;
        auto value = decode_field_value(field, field_bytes, field_error);
        if (field_error != TwimeDecodeError::Ok) {
            return field_error;
        }
        out_message.fields.push_back(DecodedTwimeField{&field, value});
    }

    return TwimeDecodeError::Ok;
}

}  // namespace moex::twime_sbe
