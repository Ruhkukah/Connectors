#include "moex/twime_sbe/twime_message_header.hpp"

#include "twime_test_support.hpp"

#include <array>
#include <iostream>

int main() {
    try {
        constexpr moex::twime_sbe::TwimeMessageHeader expected{
            .block_length = 47,
            .template_id = 6000,
            .schema_id = 19781,
            .version = 7,
        };
        constexpr auto bytes = moex::twime_sbe::encode_twime_message_header(expected);
        static_assert(bytes[0] == std::byte{0x2F});
        static_assert(bytes[1] == std::byte{0x00});
        static_assert(bytes[2] == std::byte{0x70});
        static_assert(bytes[3] == std::byte{0x17});

        moex::twime_sbe::TwimeMessageHeader decoded{};
        const auto result = moex::twime_sbe::decode_twime_message_header(bytes, decoded);
        moex::twime_sbe::test::require(result == moex::twime_sbe::TwimeDecodeError::Ok, "header decode failed");
        moex::twime_sbe::test::require(decoded.block_length == expected.block_length, "block_length mismatch");
        moex::twime_sbe::test::require(decoded.template_id == expected.template_id, "template_id mismatch");
        moex::twime_sbe::test::require(decoded.schema_id == expected.schema_id, "schema_id mismatch");
        moex::twime_sbe::test::require(decoded.version == expected.version, "version mismatch");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
