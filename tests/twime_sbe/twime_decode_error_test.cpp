#include "moex/twime_sbe/twime_codec.hpp"

#include "twime_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_sbe::TwimeCodec codec;
        auto request = moex::twime_sbe::test::make_sample_request("Terminate");
        std::vector<std::byte> bytes;
        moex::twime_sbe::test::require(
            codec.encode_message(request, bytes) == moex::twime_sbe::TwimeDecodeError::Ok,
            "failed to encode terminate message");

        moex::twime_sbe::DecodedTwimeMessage decoded;
        moex::twime_sbe::test::require(
            codec.decode_message(std::span<const std::byte>(bytes.data(), 3), decoded) == moex::twime_sbe::TwimeDecodeError::NeedMoreData,
            "NeedMoreData not returned");

        auto invalid_block_length = bytes;
        invalid_block_length[0] = std::byte{0x02};
        invalid_block_length[1] = std::byte{0x00};
        moex::twime_sbe::test::require(
            codec.decode_message(invalid_block_length, decoded) == moex::twime_sbe::TwimeDecodeError::InvalidBlockLength,
            "InvalidBlockLength not returned");

        auto unknown_template = bytes;
        unknown_template[2] = std::byte{0xFF};
        unknown_template[3] = std::byte{0x7F};
        moex::twime_sbe::test::require(
            codec.decode_message(unknown_template, decoded) == moex::twime_sbe::TwimeDecodeError::UnknownTemplateId,
            "UnknownTemplateId not returned");

        auto invalid_enum = bytes;
        invalid_enum.back() = std::byte{0xFF};
        moex::twime_sbe::test::require(
            codec.decode_message(invalid_enum, decoded) == moex::twime_sbe::TwimeDecodeError::InvalidEnumValue,
            "InvalidEnumValue not returned");

        auto trailing = bytes;
        trailing.push_back(std::byte{0x00});
        moex::twime_sbe::test::require(
            codec.decode_message(trailing, decoded) == moex::twime_sbe::TwimeDecodeError::TrailingBytes,
            "TrailingBytes not returned");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
