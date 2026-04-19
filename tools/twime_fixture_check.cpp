#include "moex/twime_sbe/twime_cert_log_formatter.hpp"
#include "moex/twime_sbe/twime_codec.hpp"

#include "twime_test_support.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string trim_trailing_newlines(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

int run_check(const fs::path& fixtures_dir, bool write_expected) {
    moex::twime_sbe::TwimeCodec codec;
    moex::twime_sbe::TwimeCertLogFormatter formatter;

    for (const auto& entry : fs::directory_iterator(fixtures_dir)) {
        if (entry.path().extension() != ".yaml") {
            continue;
        }

        const auto fixture = moex::twime_sbe::test::load_fixture(entry.path());
        const auto request = moex::twime_sbe::test::build_encode_request(fixture);
        std::vector<std::byte> bytes;
        const auto encode_error = codec.encode_message(request, bytes);
        if (encode_error != moex::twime_sbe::TwimeDecodeError::Ok) {
            std::cerr << "fixture encode failed for " << entry.path() << '\n';
            return 1;
        }

        moex::twime_sbe::DecodedTwimeMessage decoded;
        const auto decode_error = codec.decode_message(bytes, decoded);
        if (decode_error != moex::twime_sbe::TwimeDecodeError::Ok) {
            std::cerr << "fixture decode failed for " << entry.path() << '\n';
            return 1;
        }

        const auto actual_hex = moex::twime_sbe::test::bytes_to_hex(bytes);
        const auto actual_certlog = formatter.format(decoded);
        const auto stem = entry.path().stem().string();
        const auto expected_hex_path = fixtures_dir / "expected" / (stem + ".hex");
        const auto expected_certlog_path = fixtures_dir / "expected" / (stem + ".certlog");

        if (write_expected) {
            std::ofstream(expected_hex_path) << actual_hex << '\n';
            std::ofstream(expected_certlog_path) << actual_certlog << '\n';
            continue;
        }

        const auto expected_hex = trim_trailing_newlines(moex::twime_sbe::test::read_text_file(expected_hex_path));
        const auto expected_certlog =
            trim_trailing_newlines(moex::twime_sbe::test::read_text_file(expected_certlog_path));
        if (actual_hex != expected_hex) {
            std::cerr << "hex golden mismatch for " << stem << '\n';
            return 1;
        }
        if (actual_certlog != expected_certlog) {
            std::cerr << "certlog golden mismatch for " << stem << '\n';
            return 1;
        }
    }

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    fs::path fixtures_dir;
    bool check = false;
    bool write_expected = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--fixtures" && index + 1 < argc) {
            fixtures_dir = argv[++index];
            continue;
        }
        if (argument == "--check") {
            check = true;
            continue;
        }
        if (argument == "--write") {
            write_expected = true;
            continue;
        }
        std::cerr << "unknown argument: " << argument << '\n';
        return 2;
    }

    if (fixtures_dir.empty()) {
        std::cerr << "missing --fixtures\n";
        return 2;
    }
    if (check == write_expected) {
        std::cerr << "select exactly one of --check or --write\n";
        return 2;
    }

    return run_check(fixtures_dir, write_expected);
}
