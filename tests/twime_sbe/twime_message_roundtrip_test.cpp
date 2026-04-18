#include "moex/twime_sbe/twime_cert_log_formatter.hpp"
#include "moex/twime_sbe/twime_codec.hpp"
#include "moex/twime_sbe/twime_schema.hpp"

#include "twime_test_support.hpp"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    try {
        constexpr std::uint16_t required_template_ids[] = {
            5000, 5001, 5002, 5003, 5004, 5005, 5006, 5007, 5008, 5009,
            6000, 6004, 6005, 6006, 6007, 6008, 6009, 6010, 6011,
            7007, 7010, 7014, 7015, 7016, 7017, 7018, 7019, 7020,
        };
        for (const auto template_id : required_template_ids) {
            moex::twime_sbe::test::require(
                moex::twime_sbe::TwimeSchemaView::find_message_by_template_id(template_id) != nullptr,
                "required template id missing");
        }

        for (const auto& message : moex::twime_sbe::TwimeSchemaView::messages()) {
            std::size_t computed_block_length = 0;
            for (std::size_t index = 0; index < message.field_count; ++index) {
                computed_block_length += message.fields[index].encoded_size;
            }
            moex::twime_sbe::test::require(
                computed_block_length == message.block_length,
                "message block length is not derived from field metadata");
        }

        moex::twime_sbe::TwimeCodec codec;
        for (const auto& message : moex::twime_sbe::TwimeSchemaView::messages()) {
            auto request = moex::twime_sbe::test::make_sample_request(message.name);
            std::vector<std::byte> bytes;
            const auto encode_error = codec.encode_message(request, bytes);
            moex::twime_sbe::test::require(
                encode_error == moex::twime_sbe::TwimeDecodeError::Ok,
                "sample message encode failed");

            moex::twime_sbe::DecodedTwimeMessage decoded;
            const auto decode_error = codec.decode_message(bytes, decoded);
            moex::twime_sbe::test::require(
                decode_error == moex::twime_sbe::TwimeDecodeError::Ok,
                "sample message decode failed");
            moex::twime_sbe::test::require(decoded.metadata != nullptr, "decoded metadata missing");
            moex::twime_sbe::test::require(decoded.metadata->template_id == message.template_id, "decoded template mismatch");
        }

        const auto fixtures_dir = moex::twime_sbe::test::project_root() / "tests/fixtures/twime_sbe";
        for (const auto& path : fs::directory_iterator(fixtures_dir)) {
            if (path.path().extension() != ".yaml") {
                continue;
            }

            const auto fixture = moex::twime_sbe::test::load_fixture(path.path());
            const auto request = moex::twime_sbe::test::build_encode_request(fixture);
            std::vector<std::byte> bytes;
            const auto encode_error = codec.encode_message(request, bytes);
            moex::twime_sbe::test::require(
                encode_error == moex::twime_sbe::TwimeDecodeError::Ok,
                "fixture encode failed");

            const auto expected_hex = moex::twime_sbe::test::read_text_file(
                fixtures_dir / "expected" / (path.path().stem().string() + ".hex"));
            const auto trimmed_hex = expected_hex.substr(0, expected_hex.find_first_of("\r\n"));
            moex::twime_sbe::test::require(
                moex::twime_sbe::test::bytes_to_hex(bytes) == trimmed_hex,
                "fixture hex mismatch");

            moex::twime_sbe::DecodedTwimeMessage decoded;
            const auto decode_error = codec.decode_message(bytes, decoded);
            moex::twime_sbe::test::require(
                decode_error == moex::twime_sbe::TwimeDecodeError::Ok,
                "fixture decode failed");
            moex::twime_sbe::test::require(
                moex::twime_sbe::test::fixture_matches_message(fixture, decoded),
                "fixture field comparison failed");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
