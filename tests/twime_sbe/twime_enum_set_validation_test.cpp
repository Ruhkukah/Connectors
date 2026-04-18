#include "moex/twime_sbe/twime_codec.hpp"

#include "twime_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_sbe::TwimeCodec codec;

        auto enum_request = moex::twime_sbe::test::make_sample_request("NewOrderSingle");
        std::vector<std::byte> bytes;
        moex::twime_sbe::test::require(
            codec.encode_message(enum_request, bytes) == moex::twime_sbe::TwimeDecodeError::Ok,
            "valid enum name did not encode");

        for (auto& field : enum_request.fields) {
            if (field.name == "ComplianceID") {
                field.value = moex::twime_sbe::TwimeFieldValue::enum_name("UnknownCompliance");
            }
        }
        moex::twime_sbe::test::require(
            codec.encode_message(enum_request, bytes) == moex::twime_sbe::TwimeDecodeError::InvalidEnumValue,
            "invalid enum name did not fail");

        auto set_request = moex::twime_sbe::test::make_sample_request("OrderMassCancelRequest");
        for (auto& field : set_request.fields) {
            if (field.name == "SecurityType") {
                field.value = moex::twime_sbe::TwimeFieldValue::set_name("Future|Option");
            }
        }
        moex::twime_sbe::test::require(
            codec.encode_message(set_request, bytes) == moex::twime_sbe::TwimeDecodeError::Ok,
            "valid set token list did not encode");

        for (auto& field : set_request.fields) {
            if (field.name == "SecurityType") {
                field.value = moex::twime_sbe::TwimeFieldValue::set_name("Future|Bogus");
            }
        }
        moex::twime_sbe::test::require(
            codec.encode_message(set_request, bytes) == moex::twime_sbe::TwimeDecodeError::InvalidSetValue,
            "mixed valid/invalid set token list did not fail");

        for (auto& field : set_request.fields) {
            if (field.name == "SecurityType") {
                field.value = moex::twime_sbe::TwimeFieldValue::set_name("Bogus");
            }
        }
        moex::twime_sbe::test::require(
            codec.encode_message(set_request, bytes) == moex::twime_sbe::TwimeDecodeError::InvalidSetValue,
            "invalid set token did not fail");

        for (auto& field : set_request.fields) {
            if (field.name == "SecurityType") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(0x80);
            }
        }
        moex::twime_sbe::test::require(
            codec.encode_message(set_request, bytes) == moex::twime_sbe::TwimeDecodeError::InvalidSetValue,
            "invalid raw set mask did not fail");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
