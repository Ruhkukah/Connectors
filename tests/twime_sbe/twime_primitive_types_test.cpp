#include "moex/twime_sbe/twime_codec.hpp"
#include "moex/twime_sbe/twime_types.hpp"

#include "twime_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_sbe::String7 account{};
        account.assign("AAAA");
        moex::twime_sbe::test::require(account.trimmed_view() == "AAAA", "String7 trimming failed");

        auto timestamp = moex::twime_sbe::TwimeFieldValue::timestamp(moex::twime_sbe::kTwimeTimestampNull);
        moex::twime_sbe::test::require(
            timestamp.unsigned_value == moex::twime_sbe::kTwimeTimestampNull,
            "TimeStamp null round-trip failed");

        auto decimal = moex::twime_sbe::TwimeFieldValue::decimal(100000);
        moex::twime_sbe::test::require(decimal.decimal5.mantissa == 100000, "Decimal5 mantissa mismatch");

        moex::twime_sbe::TwimeCodec codec;
        auto request = moex::twime_sbe::test::make_sample_request("NewOrderSingle");
        for (auto& field : request.fields) {
            if (field.name == "TimeInForce") {
                field.value = moex::twime_sbe::TwimeFieldValue::unsigned_integer(255);
            }
        }

        std::vector<std::byte> bytes;
        auto encode_error = codec.encode_message(request, bytes);
        moex::twime_sbe::test::require(
            encode_error == moex::twime_sbe::TwimeDecodeError::InvalidEnumValue,
            "invalid enum was not rejected");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
