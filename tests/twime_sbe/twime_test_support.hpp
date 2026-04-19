#pragma once

#include "moex/twime_sbe/twime_cert_log_formatter.hpp"
#include "moex/twime_sbe/twime_codec.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace moex::twime_sbe::test {

struct FixtureFieldSpec {
    std::string name;
    std::string scalar;
    bool has_decimal_mantissa{false};
    std::int64_t decimal_mantissa{0};
};

struct FixtureSpec {
    std::string message_name;
    std::uint16_t template_id{0};
    std::uint16_t schema_id{0};
    std::uint16_t version{0};
    std::vector<FixtureFieldSpec> fields;
};

[[nodiscard]] std::filesystem::path project_root();
[[nodiscard]] FixtureSpec load_fixture(const std::filesystem::path& path);
[[nodiscard]] std::string read_text_file(const std::filesystem::path& path);
[[nodiscard]] std::vector<std::byte> bytes_from_hex(const std::string& hex);
[[nodiscard]] std::string bytes_to_hex(std::span<const std::byte> bytes);
[[nodiscard]] TwimeEncodeRequest build_encode_request(const FixtureSpec& fixture);
[[nodiscard]] bool fixture_matches_message(const FixtureSpec& fixture, const DecodedTwimeMessage& message);
[[nodiscard]] TwimeEncodeRequest make_sample_request(std::string_view message_name);
void require(bool condition, const std::string& message);

} // namespace moex::twime_sbe::test
