#include "moex/twime_sbe/twime_codec.hpp"
#include "moex/twime_sbe/twime_frame_assembler.hpp"

#include "twime_test_support.hpp"

#include <iostream>

int main() {
    try {
        moex::twime_sbe::TwimeCodec codec;
        auto request = moex::twime_sbe::test::make_sample_request("Sequence");
        std::vector<std::byte> one_message;
        moex::twime_sbe::test::require(
            codec.encode_message(request, one_message) == moex::twime_sbe::TwimeDecodeError::Ok,
            "failed to encode base sequence message");

        moex::twime_sbe::TwimeFrameAssembler assembler(256);
        auto result = assembler.feed(one_message);
        moex::twime_sbe::test::require(result.frames_ready == 1, "single chunk frame not assembled");
        moex::twime_sbe::test::require(assembler.has_frame(), "frame queue empty");
        moex::twime_sbe::test::require(assembler.pop_frame().bytes == one_message, "single frame mismatch");

        assembler.reset();
        std::vector<std::byte> batched = one_message;
        batched.insert(batched.end(), one_message.begin(), one_message.end());
        result = assembler.feed(batched);
        moex::twime_sbe::test::require(result.frames_ready == 2, "batched frames not assembled");
        moex::twime_sbe::test::require(assembler.pop_frame().bytes == one_message, "first batched frame mismatch");
        moex::twime_sbe::test::require(assembler.pop_frame().bytes == one_message, "second batched frame mismatch");

        assembler.reset();
        for (const auto byte : one_message) {
            result = assembler.feed(std::span<const std::byte>(&byte, 1));
        }
        moex::twime_sbe::test::require(result.frames_ready == 1, "byte-wise frame assembly failed");

        assembler.reset();
        result = assembler.feed(std::span<const std::byte>(one_message.data(), 4));
        moex::twime_sbe::test::require(result.error == moex::twime_sbe::TwimeDecodeError::NeedMoreData, "incomplete header not reported");

        assembler.reset();
        result = assembler.feed(std::span<const std::byte>(one_message.data(), 10));
        moex::twime_sbe::test::require(result.error == moex::twime_sbe::TwimeDecodeError::NeedMoreData, "incomplete body not reported");

        assembler.reset();
        auto invalid_schema = one_message;
        invalid_schema[4] = std::byte{0x00};
        invalid_schema[5] = std::byte{0x00};
        result = assembler.feed(invalid_schema);
        moex::twime_sbe::test::require(result.error == moex::twime_sbe::TwimeDecodeError::UnsupportedSchemaId, "invalid schema id not rejected");

        assembler.reset();
        auto invalid_version = one_message;
        invalid_version[6] = std::byte{0x01};
        invalid_version[7] = std::byte{0x00};
        result = assembler.feed(invalid_version);
        moex::twime_sbe::test::require(result.error == moex::twime_sbe::TwimeDecodeError::UnsupportedVersion, "invalid version not rejected");

        moex::twime_sbe::TwimeFrameAssembler small_assembler(8);
        result = small_assembler.feed(one_message);
        moex::twime_sbe::test::require(result.error == moex::twime_sbe::TwimeDecodeError::BufferTooSmall, "oversized frame not rejected");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
