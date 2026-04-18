#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace moex::twime_sbe {

enum class TwimeFieldKind {
    Primitive,
    String,
    Decimal5,
    Enum,
    Set,
    TimeStamp,
    DeltaMillisecs,
    Composite,
};

enum class TwimePrimitiveType {
    None,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Char,
};

enum class TwimeLayer {
    Session,
    Application,
};

enum class TwimeDirection {
    ClientToServer,
    ServerToClient,
    Bidirectional,
    Unknown,
};

struct TwimeEnumValueMetadata {
    std::string_view name;
    std::int64_t wire_value;
    std::string_view wire_text;
};

struct TwimeEnumMetadata {
    std::string_view name;
    TwimePrimitiveType primitive_type;
    std::size_t encoded_size;
    const TwimeEnumValueMetadata* values;
    std::size_t value_count;
};

struct TwimeSetChoiceMetadata {
    std::string_view name;
    std::uint8_t bit_index;
    std::string_view description;
};

struct TwimeSetMetadata {
    std::string_view name;
    TwimePrimitiveType primitive_type;
    std::size_t encoded_size;
    const TwimeSetChoiceMetadata* choices;
    std::size_t choice_count;
};

struct TwimeTypeMetadata {
    std::string_view name;
    TwimeFieldKind kind;
    TwimePrimitiveType primitive_type;
    std::size_t encoded_size;
    bool nullable;
    std::uint64_t null_value;
    bool has_null_value;
    std::size_t fixed_length;
    int constant_exponent;
    const TwimeEnumMetadata* enum_metadata;
    const TwimeSetMetadata* set_metadata;
    std::string_view description;
};

struct TwimeFieldMetadata {
    std::string_view name;
    std::uint16_t field_id;
    const TwimeTypeMetadata* type;
    std::size_t encoded_size;
    std::size_t offset;
    bool nullable;
    std::uint64_t null_value;
    bool has_null_value;
};

struct TwimeMessageMetadata {
    std::string_view name;
    std::uint16_t template_id;
    TwimeLayer layer;
    TwimeDirection direction;
    std::size_t block_length;
    const TwimeFieldMetadata* fields;
    std::size_t field_count;
};

struct TwimeSchemaInfo {
    std::string_view package;
    std::uint16_t schema_id;
    std::uint16_t schema_version;
    std::string_view byte_order;
    std::string_view sha256;
};

namespace generated {
const TwimeSchemaInfo& schema_info() noexcept;
std::span<const TwimeTypeMetadata> types() noexcept;
std::span<const TwimeEnumMetadata> enums() noexcept;
std::span<const TwimeSetMetadata> sets() noexcept;
std::span<const TwimeMessageMetadata> messages() noexcept;
const TwimeTypeMetadata* find_type(std::string_view name) noexcept;
const TwimeEnumMetadata* find_enum(std::string_view name) noexcept;
const TwimeSetMetadata* find_set(std::string_view name) noexcept;
const TwimeMessageMetadata* find_message_by_name(std::string_view name) noexcept;
const TwimeMessageMetadata* find_message_by_template_id(std::uint16_t template_id) noexcept;
}  // namespace generated

struct TwimeSchemaView {
    static const TwimeSchemaInfo& info() noexcept { return generated::schema_info(); }
    static std::span<const TwimeTypeMetadata> types() noexcept { return generated::types(); }
    static std::span<const TwimeEnumMetadata> enums() noexcept { return generated::enums(); }
    static std::span<const TwimeSetMetadata> sets() noexcept { return generated::sets(); }
    static std::span<const TwimeMessageMetadata> messages() noexcept { return generated::messages(); }
    static const TwimeTypeMetadata* find_type(std::string_view name) noexcept { return generated::find_type(name); }
    static const TwimeEnumMetadata* find_enum(std::string_view name) noexcept { return generated::find_enum(name); }
    static const TwimeSetMetadata* find_set(std::string_view name) noexcept { return generated::find_set(name); }
    static const TwimeMessageMetadata* find_message_by_name(std::string_view name) noexcept {
        return generated::find_message_by_name(name);
    }
    static const TwimeMessageMetadata* find_message_by_template_id(std::uint16_t template_id) noexcept {
        return generated::find_message_by_template_id(template_id);
    }
};

}  // namespace moex::twime_sbe
