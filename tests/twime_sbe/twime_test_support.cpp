#include "twime_test_support.hpp"

#include "moex/twime_sbe/twime_schema.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace moex::twime_sbe::test {

namespace {

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

std::pair<std::string, std::string> split_key_value(std::string_view line) {
    const auto separator = line.find(':');
    if (separator == std::string_view::npos) {
        return {trim_copy(line), {}};
    }
    return {
        trim_copy(line.substr(0, separator)),
        trim_copy(line.substr(separator + 1)),
    };
}

std::string unquote(std::string value) {
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool is_integer_text(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    std::size_t index = 0;
    if (value.front() == '-') {
        if (value.size() == 1) {
            return false;
        }
        index = 1;
    }
    for (; index < value.size(); ++index) {
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

const FixtureFieldSpec* find_fixture_field(const FixtureSpec& fixture, std::string_view name) {
    for (const auto& field : fixture.fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

const DecodedTwimeField* find_decoded_field(const DecodedTwimeMessage& message, std::string_view name) {
    for (const auto& field : message.fields) {
        if (field.metadata != nullptr && field.metadata->name == name) {
            return &field;
        }
    }
    return nullptr;
}

const TwimeEnumValueMetadata* find_enum_by_name(const TwimeEnumMetadata& metadata, std::string_view name) {
    for (std::size_t index = 0; index < metadata.value_count; ++index) {
        if (metadata.values[index].name == name) {
            return metadata.values + index;
        }
    }
    return nullptr;
}

std::uint64_t parse_unsigned(std::string_view text) {
    return static_cast<std::uint64_t>(std::stoull(std::string(text)));
}

std::int64_t parse_signed(std::string_view text) {
    return static_cast<std::int64_t>(std::stoll(std::string(text)));
}

TwimeFieldValue sample_value_for_field(const TwimeFieldMetadata& field) {
    const auto& type = *field.type;
    if (field.name == "ClOrdID") {
        return TwimeFieldValue::unsigned_integer(102);
    }
    if (field.name == "ExpireDate") {
        return TwimeFieldValue::timestamp(kTwimeTimestampNull);
    }
    if (field.name == "Timestamp" || field.name == "RequestTimestamp") {
        return TwimeFieldValue::timestamp(1'715'000'000'000'000'000ULL);
    }
    if (field.name == "KeepaliveInterval") {
        return TwimeFieldValue::delta_millisecs(1000);
    }
    if (field.name == "Credentials") {
        return TwimeFieldValue::string("LOGIN");
    }
    if (field.name == "Price" || field.name == "LastPx" || field.name == "LegPrice") {
        return TwimeFieldValue::decimal(100000);
    }
    if (field.name == "SecurityID") {
        return TwimeFieldValue::signed_integer(347990);
    }
    if (field.name == "ClOrdLinkID") {
        return TwimeFieldValue::signed_integer(7895424);
    }
    if (field.name == "OrderQty") {
        return TwimeFieldValue::unsigned_integer(5);
    }
    if (field.name == "ComplianceID") {
        return TwimeFieldValue::enum_name("Algorithm");
    }
    if (field.name == "TimeInForce") {
        return TwimeFieldValue::enum_name("Day");
    }
    if (field.name == "Side") {
        return TwimeFieldValue::enum_name("Buy");
    }
    if (field.name == "ClientFlags" || field.name == "Flags" || field.name == "Flags2") {
        return TwimeFieldValue::unsigned_integer(0);
    }
    if (field.name == "Account") {
        return TwimeFieldValue::string("AAAA");
    }
    if (field.name == "SecurityGroup") {
        return TwimeFieldValue::string("FUT");
    }
    if (field.name == "SecurityType") {
        return TwimeFieldValue::set_name("Future");
    }
    if (field.name == "OrderID") {
        return TwimeFieldValue::signed_integer(9001001);
    }
    if (field.name == "PrevOrderID") {
        return TwimeFieldValue::signed_integer(9001000);
    }
    if (field.name == "DisplayOrderID") {
        return TwimeFieldValue::signed_integer(9001002);
    }
    if (field.name == "DisplayQty") {
        return TwimeFieldValue::unsigned_integer(1);
    }
    if (field.name == "DisplayVarianceQty") {
        return TwimeFieldValue::unsigned_integer(0);
    }
    if (field.name == "Mode") {
        return TwimeFieldValue::enum_name("ChangeOrderQty");
    }
    if (field.name == "TotalAffectedOrders") {
        return TwimeFieldValue::signed_integer(2);
    }
    if (field.name == "TradingSessionID") {
        return TwimeFieldValue::signed_integer(1200);
    }
    if (field.name == "EventId") {
        return TwimeFieldValue::signed_integer(7001);
    }
    if (field.name == "TradSesEvent") {
        return TwimeFieldValue::enum_name("SessionDataReady");
    }
    if (field.name == "TrdMatchID") {
        return TwimeFieldValue::signed_integer(456789);
    }
    if (field.name == "LastQty") {
        return TwimeFieldValue::unsigned_integer(1);
    }
    if (field.name == "FromSeqNo") {
        return TwimeFieldValue::unsigned_integer(21);
    }
    if (field.name == "NextSeqNo") {
        return TwimeFieldValue::unsigned_integer(26);
    }
    if (field.name == "Count") {
        return TwimeFieldValue::unsigned_integer(5);
    }
    if (field.name == "TerminationCode") {
        return TwimeFieldValue::enum_name("Finished");
    }
    if (field.name == "EstablishmentRejectCode") {
        return TwimeFieldValue::enum_name("Credentials");
    }
    if (field.name == "QueueSize") {
        return TwimeFieldValue::unsigned_integer(2);
    }
    if (field.name == "PenaltyRemain") {
        return TwimeFieldValue::unsigned_integer(1000);
    }
    if (field.name == "RefTagID") {
        return TwimeFieldValue::unsigned_integer(59);
    }
    if (field.name == "SessionRejectReason") {
        return TwimeFieldValue::enum_name("Other");
    }
    if (field.name == "OrdRejReason") {
        return TwimeFieldValue::signed_integer(-12);
    }

    switch (type.kind) {
        case TwimeFieldKind::Primitive:
            if (type.primitive_type == TwimePrimitiveType::Int8 || type.primitive_type == TwimePrimitiveType::Int16 ||
                type.primitive_type == TwimePrimitiveType::Int32 || type.primitive_type == TwimePrimitiveType::Int64) {
                return TwimeFieldValue::signed_integer(1);
            }
            return TwimeFieldValue::unsigned_integer(1);
        case TwimeFieldKind::String:
            return TwimeFieldValue::string("X");
        case TwimeFieldKind::TimeStamp:
            return TwimeFieldValue::timestamp(1'715'000'000'000'000'000ULL);
        case TwimeFieldKind::DeltaMillisecs:
            return TwimeFieldValue::delta_millisecs(1000);
        case TwimeFieldKind::Decimal5:
            return TwimeFieldValue::decimal(100000);
        case TwimeFieldKind::Enum:
            if (type.enum_metadata != nullptr && type.enum_metadata->value_count > 0) {
                return TwimeFieldValue::enum_name(type.enum_metadata->values[0].name);
            }
            return TwimeFieldValue::unsigned_integer(0);
        case TwimeFieldKind::Set:
            if (type.set_metadata != nullptr && type.set_metadata->choice_count > 0) {
                return TwimeFieldValue::set_name(type.set_metadata->choices[0].name);
            }
            return TwimeFieldValue::unsigned_integer(0);
        case TwimeFieldKind::Composite:
        default:
            return TwimeFieldValue::unsigned_integer(0);
    }
}

}  // namespace

std::filesystem::path project_root() {
    return std::filesystem::path(MOEX_SOURCE_ROOT);
}

FixtureSpec load_fixture(const std::filesystem::path& path) {
    FixtureSpec fixture;
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("unable to open fixture: " + path.string());
    }

    std::string line;
    FixtureFieldSpec* nested_field = nullptr;
    while (std::getline(input, line)) {
        const auto trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const auto indent = line.find_first_not_of(' ');
        const auto level = indent == std::string::npos ? 0U : static_cast<unsigned int>(indent);
        const auto [key, value] = split_key_value(trimmed);
        if (level == 0U) {
            nested_field = nullptr;
            if (key == "message") {
                fixture.message_name = unquote(value);
            } else if (key == "template_id") {
                fixture.template_id = static_cast<std::uint16_t>(parse_unsigned(value));
            } else if (key == "schema_id") {
                fixture.schema_id = static_cast<std::uint16_t>(parse_unsigned(value));
            } else if (key == "version") {
                fixture.version = static_cast<std::uint16_t>(parse_unsigned(value));
            }
            continue;
        }
        if (level == 2U) {
            fixture.fields.push_back(FixtureFieldSpec{.name = key, .scalar = unquote(value)});
            nested_field = &fixture.fields.back();
            continue;
        }
        if (level == 4U && nested_field != nullptr && key == "mantissa") {
            nested_field->has_decimal_mantissa = true;
            nested_field->decimal_mantissa = parse_signed(value);
        }
    }

    return fixture;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("unable to open file: " + path.string());
    }
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

std::vector<std::byte> bytes_from_hex(const std::string& hex) {
    std::vector<std::byte> bytes;
    std::string normalized;
    normalized.reserve(hex.size());
    for (char ch : hex) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            normalized.push_back(ch);
        }
    }
    if ((normalized.size() % 2U) != 0U) {
        throw std::runtime_error("hex string must contain an even number of nybbles");
    }
    bytes.reserve(normalized.size() / 2U);
    for (std::size_t index = 0; index < normalized.size(); index += 2U) {
        const auto value = static_cast<unsigned int>(std::stoul(normalized.substr(index, 2), nullptr, 16));
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

std::string bytes_to_hex(std::span<const std::byte> bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output;
    output.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned int>(byte);
        output.push_back(kHex[(value >> 4U) & 0xFU]);
        output.push_back(kHex[value & 0xFU]);
    }
    return output;
}

TwimeEncodeRequest build_encode_request(const FixtureSpec& fixture) {
    TwimeEncodeRequest request;
    request.message_name = fixture.message_name;
    request.template_id = fixture.template_id;

    const auto* metadata = TwimeSchemaView::find_message_by_name(fixture.message_name);
    if (metadata == nullptr) {
        throw std::runtime_error("unknown TWIME message in fixture: " + fixture.message_name);
    }

    request.fields.reserve(fixture.fields.size());
    for (const auto& field : fixture.fields) {
        const auto* field_metadata = [&]() -> const TwimeFieldMetadata* {
            for (std::size_t index = 0; index < metadata->field_count; ++index) {
                if (metadata->fields[index].name == field.name) {
                    return metadata->fields + index;
                }
            }
            return nullptr;
        }();
        if (field_metadata == nullptr) {
            throw std::runtime_error("unknown fixture field: " + field.name);
        }

        const auto& type = *field_metadata->type;
        TwimeFieldValue value;
        switch (type.kind) {
            case TwimeFieldKind::Primitive:
                if (type.primitive_type == TwimePrimitiveType::Int8 || type.primitive_type == TwimePrimitiveType::Int16 ||
                    type.primitive_type == TwimePrimitiveType::Int32 || type.primitive_type == TwimePrimitiveType::Int64) {
                    value = TwimeFieldValue::signed_integer(parse_signed(field.scalar));
                } else {
                    value = TwimeFieldValue::unsigned_integer(parse_unsigned(field.scalar));
                }
                break;
            case TwimeFieldKind::DeltaMillisecs:
                value = TwimeFieldValue::delta_millisecs(static_cast<std::uint32_t>(parse_unsigned(field.scalar)));
                break;
            case TwimeFieldKind::TimeStamp:
                value = TwimeFieldValue::timestamp(parse_unsigned(field.scalar));
                break;
            case TwimeFieldKind::Decimal5:
                value = TwimeFieldValue::decimal(field.decimal_mantissa);
                break;
            case TwimeFieldKind::String:
                value = TwimeFieldValue::string(field.scalar);
                break;
            case TwimeFieldKind::Enum:
                value = is_integer_text(field.scalar)
                    ? TwimeFieldValue::unsigned_integer(parse_unsigned(field.scalar))
                    : TwimeFieldValue::enum_name(field.scalar);
                break;
            case TwimeFieldKind::Set:
                value = is_integer_text(field.scalar)
                    ? TwimeFieldValue::unsigned_integer(parse_unsigned(field.scalar))
                    : TwimeFieldValue::set_name(field.scalar);
                break;
            case TwimeFieldKind::Composite:
            default:
                throw std::runtime_error("unsupported fixture field kind for " + field.name);
        }
        request.fields.push_back(TwimeFieldInput{field.name, value});
    }

    return request;
}

bool fixture_matches_message(const FixtureSpec& fixture, const DecodedTwimeMessage& message) {
    if (message.metadata == nullptr || message.metadata->name != fixture.message_name) {
        return false;
    }
    if (message.header.template_id != fixture.template_id ||
        message.header.schema_id != fixture.schema_id ||
        message.header.version != fixture.version) {
        return false;
    }

    for (const auto& expected_field : fixture.fields) {
        const auto* decoded_field = find_decoded_field(message, expected_field.name);
        if (decoded_field == nullptr || decoded_field->metadata == nullptr) {
            return false;
        }
        const auto& type = *decoded_field->metadata->type;
        switch (type.kind) {
            case TwimeFieldKind::Primitive:
                if (type.primitive_type == TwimePrimitiveType::Int8 || type.primitive_type == TwimePrimitiveType::Int16 ||
                    type.primitive_type == TwimePrimitiveType::Int32 || type.primitive_type == TwimePrimitiveType::Int64) {
                    if (decoded_field->value.signed_value != parse_signed(expected_field.scalar)) {
                        return false;
                    }
                } else if (decoded_field->value.unsigned_value != parse_unsigned(expected_field.scalar)) {
                    return false;
                }
                break;
            case TwimeFieldKind::DeltaMillisecs:
            case TwimeFieldKind::TimeStamp:
            case TwimeFieldKind::Set:
            case TwimeFieldKind::Enum:
                if (is_integer_text(expected_field.scalar)) {
                    if (decoded_field->value.unsigned_value != parse_unsigned(expected_field.scalar)) {
                        return false;
                    }
                } else if (type.kind == TwimeFieldKind::Enum) {
                    if (type.enum_metadata == nullptr) {
                        return false;
                    }
                    const auto* expected_value = find_enum_by_name(*type.enum_metadata, expected_field.scalar);
                    if (expected_value == nullptr ||
                        decoded_field->value.unsigned_value != static_cast<std::uint64_t>(expected_value->wire_value)) {
                        return false;
                    }
                } else if (type.kind == TwimeFieldKind::Set) {
                    if (type.set_metadata == nullptr) {
                        return false;
                    }
                    std::uint64_t expected_mask = 0;
                    std::size_t start = 0;
                    const std::string_view names = expected_field.scalar;
                    while (start <= names.size()) {
                        const auto next = names.find('|', start);
                        const auto token = trim_copy(names.substr(
                            start,
                            next == std::string_view::npos ? names.size() - start : next - start));
                        for (std::size_t index = 0; index < type.set_metadata->choice_count; ++index) {
                            if (type.set_metadata->choices[index].name == token) {
                                expected_mask |= (static_cast<std::uint64_t>(1) << type.set_metadata->choices[index].bit_index);
                            }
                        }
                        if (next == std::string_view::npos) {
                            break;
                        }
                        start = next + 1;
                    }
                    if (decoded_field->value.unsigned_value != expected_mask) {
                        return false;
                    }
                }
                break;
            case TwimeFieldKind::Decimal5:
                if (!expected_field.has_decimal_mantissa ||
                    decoded_field->value.decimal5.mantissa != expected_field.decimal_mantissa) {
                    return false;
                }
                break;
            case TwimeFieldKind::String:
                if (decoded_field->value.string_view() != expected_field.scalar) {
                    return false;
                }
                break;
            case TwimeFieldKind::Composite:
            default:
                return false;
        }
    }

    return true;
}

TwimeEncodeRequest make_sample_request(std::string_view message_name) {
    const auto* metadata = TwimeSchemaView::find_message_by_name(message_name);
    if (metadata == nullptr) {
        throw std::runtime_error("unknown TWIME sample request message");
    }
    TwimeEncodeRequest request;
    request.message_name = std::string(message_name);
    request.template_id = metadata->template_id;
    request.fields.reserve(metadata->field_count);
    for (std::size_t index = 0; index < metadata->field_count; ++index) {
        const auto& field = metadata->fields[index];
        request.fields.push_back(TwimeFieldInput{std::string(field.name), sample_value_for_field(field)});
    }
    return request;
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace moex::twime_sbe::test
