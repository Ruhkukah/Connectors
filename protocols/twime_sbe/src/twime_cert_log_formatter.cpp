#include "moex/twime_sbe/twime_cert_log_formatter.hpp"

#include "moex/twime_sbe/twime_schema.hpp"

#include <sstream>

namespace moex::twime_sbe {

namespace {

const TwimeEnumValueMetadata* find_enum_value(const TwimeEnumMetadata& metadata, std::uint64_t wire_value) {
    for (std::size_t index = 0; index < metadata.value_count; ++index) {
        if (metadata.values[index].wire_value == static_cast<std::int64_t>(wire_value)) {
            return metadata.values + index;
        }
    }
    return nullptr;
}

std::string format_set(const TwimeSetMetadata& metadata, std::uint64_t mask) {
    if (mask == 0) {
        return "0";
    }
    std::ostringstream output;
    bool first = true;
    for (std::size_t index = 0; index < metadata.choice_count; ++index) {
        const auto bit = static_cast<std::uint64_t>(1) << metadata.choices[index].bit_index;
        if ((mask & bit) == 0) {
            continue;
        }
        if (!first) {
            output << '|';
        }
        output << metadata.choices[index].name;
        first = false;
    }
    if (first) {
        output << mask;
    }
    return output.str();
}

std::string format_field(const DecodedTwimeField& field) {
    const auto& type = *field.metadata->type;
    switch (type.kind) {
        case TwimeFieldKind::Primitive:
            if (type.primitive_type == TwimePrimitiveType::Int8 || type.primitive_type == TwimePrimitiveType::Int16 ||
                type.primitive_type == TwimePrimitiveType::Int32 || type.primitive_type == TwimePrimitiveType::Int64) {
                return std::to_string(field.value.signed_value);
            }
            return std::to_string(field.value.unsigned_value);
        case TwimeFieldKind::DeltaMillisecs:
            return std::to_string(field.value.unsigned_value);
        case TwimeFieldKind::TimeStamp:
            if (field.value.unsigned_value == kTwimeTimestampNull) {
                return "null";
            }
            return std::to_string(field.value.unsigned_value);
        case TwimeFieldKind::Decimal5:
            return std::to_string(field.value.decimal5.mantissa) + "e-5";
        case TwimeFieldKind::String:
            return "\"" + std::string(field.value.string_view()) + "\"";
        case TwimeFieldKind::Enum: {
            if (type.enum_metadata == nullptr) {
                return std::to_string(field.value.unsigned_value);
            }
            const auto* value = find_enum_value(*type.enum_metadata, field.value.unsigned_value);
            return value == nullptr ? std::to_string(field.value.unsigned_value) : std::string(value->name);
        }
        case TwimeFieldKind::Set:
            if (type.set_metadata == nullptr) {
                return std::to_string(field.value.unsigned_value);
            }
            return format_set(*type.set_metadata, field.value.unsigned_value);
        case TwimeFieldKind::Composite:
        default:
            return "<unsupported>";
    }
}

}  // namespace

std::string TwimeCertLogFormatter::format(const DecodedTwimeMessage& message) const {
    std::ostringstream output;
    output << message.metadata->name << " (blockLength=" << message.header.block_length
           << ", templateId=" << message.header.template_id << ", schemaId=" << message.header.schema_id
           << ", version=" << message.header.version;
    for (const auto& field : message.fields) {
        output << ", " << field.metadata->name << "=" << format_field(field);
    }
    output << ")";
    return output.str();
}

}  // namespace moex::twime_sbe
