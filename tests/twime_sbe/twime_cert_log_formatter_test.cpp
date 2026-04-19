#include "moex/twime_sbe/twime_cert_log_formatter.hpp"
#include "moex/twime_sbe/twime_codec.hpp"

#include "twime_test_support.hpp"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    try {
        const auto fixtures_dir = moex::twime_sbe::test::project_root() / "tests/fixtures/twime_sbe";
        const std::string fixture_names[] = {
            "new_order_single_day",    "sequence_heartbeat",      "retransmit_request_last5",
            "business_message_reject", "execution_single_report",
        };

        moex::twime_sbe::TwimeCodec codec;
        moex::twime_sbe::TwimeCertLogFormatter formatter;
        for (const auto& fixture_name : fixture_names) {
            const auto fixture = moex::twime_sbe::test::load_fixture(fixtures_dir / (fixture_name + ".yaml"));
            const auto request = moex::twime_sbe::test::build_encode_request(fixture);
            std::vector<std::byte> bytes;
            moex::twime_sbe::test::require(codec.encode_message(request, bytes) ==
                                               moex::twime_sbe::TwimeDecodeError::Ok,
                                           "failed to encode fixture for cert log");
            moex::twime_sbe::DecodedTwimeMessage decoded;
            moex::twime_sbe::test::require(codec.decode_message(bytes, decoded) ==
                                               moex::twime_sbe::TwimeDecodeError::Ok,
                                           "failed to decode fixture for cert log");
            const auto actual = formatter.format(decoded);
            auto expected =
                moex::twime_sbe::test::read_text_file(fixtures_dir / "expected" / (fixture_name + ".certlog"));
            if (!expected.empty() && (expected.back() == '\n' || expected.back() == '\r')) {
                expected.erase(expected.find_last_not_of("\r\n") + 1);
            }
            moex::twime_sbe::test::require(actual == expected, "cert log mismatch for " + fixture_name);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
