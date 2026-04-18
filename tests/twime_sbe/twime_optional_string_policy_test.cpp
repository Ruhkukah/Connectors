#include "moex/twime_sbe/twime_codec.hpp"

#include "twime_test_support.hpp"

#include <algorithm>
#include <iostream>

int main() {
    try {
        moex::twime_sbe::TwimeCodec codec;
        std::vector<std::byte> bytes;

        moex::twime_sbe::TwimeEncodeRequest establish_request;
        establish_request.message_name = "Establish";
        establish_request.template_id = 5000;
        establish_request.fields.push_back({"KeepaliveInterval", moex::twime_sbe::TwimeFieldValue::delta_millisecs(1000)});
        establish_request.fields.push_back({"Credentials", moex::twime_sbe::TwimeFieldValue::string("LOGIN")});
        moex::twime_sbe::test::require(
            codec.encode_message(establish_request, bytes) == moex::twime_sbe::TwimeDecodeError::Ok,
            "optional timestamp omission did not encode");
        moex::twime_sbe::DecodedTwimeMessage decoded;
        moex::twime_sbe::test::require(
            codec.decode_message(bytes, decoded) == moex::twime_sbe::TwimeDecodeError::Ok,
            "decode failed for optional timestamp omission");
        moex::twime_sbe::test::require(
            moex::twime_sbe::test::fixture_matches_message(
                moex::twime_sbe::test::FixtureSpec{
                    .message_name = "Establish",
                    .template_id = 5000,
                    .schema_id = 19781,
                    .version = 7,
                    .fields =
                        {
                            {"Timestamp", std::to_string(moex::twime_sbe::kTwimeTimestampNull)},
                            {"KeepaliveInterval", "1000"},
                            {"Credentials", "LOGIN"},
                        },
                },
                decoded),
            "optional timestamp did not default to TWIME null");

        auto missing_required = moex::twime_sbe::test::make_sample_request("NewOrderSingle");
        missing_required.fields.erase(
            std::remove_if(
                missing_required.fields.begin(),
                missing_required.fields.end(),
                [](const auto& field) { return field.name == "Price"; }),
            missing_required.fields.end());
        moex::twime_sbe::test::require(
            codec.encode_message(missing_required, bytes) == moex::twime_sbe::TwimeDecodeError::InvalidFieldValue,
            "required Decimal5 field omission did not fail");

        auto long_account = moex::twime_sbe::test::make_sample_request("NewOrderSingle");
        for (auto& field : long_account.fields) {
            if (field.name == "Account") {
                field.value = moex::twime_sbe::TwimeFieldValue::string("TOO-LONG");
            }
        }
        moex::twime_sbe::test::require(
            codec.encode_message(long_account, bytes) == moex::twime_sbe::TwimeDecodeError::InvalidStringEncoding,
            "overlength fixed string did not fail");

        auto embedded_zero = moex::twime_sbe::test::make_sample_request("NewOrderSingle");
        for (auto& field : embedded_zero.fields) {
            if (field.name == "Account") {
                field.value = moex::twime_sbe::TwimeFieldValue::string(std::string("AA\0A", 4));
            }
        }
        moex::twime_sbe::test::require(
            codec.encode_message(embedded_zero, bytes) == moex::twime_sbe::TwimeDecodeError::Ok,
            "embedded zero fixed string did not encode");
        moex::twime_sbe::test::require(
            codec.decode_message(bytes, decoded) == moex::twime_sbe::TwimeDecodeError::Ok,
            "embedded zero fixed string did not decode");
        const auto* account_field = [&decoded]() -> const moex::twime_sbe::DecodedTwimeField* {
            for (const auto& field : decoded.fields) {
                if (field.metadata != nullptr && field.metadata->name == "Account") {
                    return &field;
                }
            }
            return nullptr;
        }();
        moex::twime_sbe::test::require(account_field != nullptr, "decoded Account field missing");
        moex::twime_sbe::test::require(account_field->value.string_view().size() == 4, "embedded zero length not preserved");
        moex::twime_sbe::test::require(account_field->value.string_view()[2] == '\0', "embedded zero not preserved");

        auto invalid_string = bytes;
        const auto* metadata = moex::twime_sbe::TwimeSchemaView::find_message_by_name("NewOrderSingle");
        const auto* account_meta = [&metadata]() -> const moex::twime_sbe::TwimeFieldMetadata* {
            for (std::size_t index = 0; index < metadata->field_count; ++index) {
                if (metadata->fields[index].name == "Account") {
                    return metadata->fields + index;
                }
            }
            return nullptr;
        }();
        moex::twime_sbe::test::require(account_meta != nullptr, "Account metadata missing");
        invalid_string[moex::twime_sbe::kTwimeMessageHeaderSize + account_meta->offset] = std::byte{0x01};
        moex::twime_sbe::test::require(
            codec.decode_message(invalid_string, decoded) == moex::twime_sbe::TwimeDecodeError::InvalidStringEncoding,
            "invalid fixed-string byte did not fail decode");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
