#include "twime_test_support.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

} // namespace

int main() {
    try {
        const auto root = std::filesystem::path(MOEX_SOURCE_ROOT);
        const auto template_text = read_text(root / "profiles" / "test_twime_live_session.template.yaml");
        moex::twime_sbe::test::require(template_text.find("TEST_ENDPOINT_HOST_PLACEHOLDER") != std::string::npos,
                                       "tracked live-session template must keep a placeholder host");
        moex::twime_sbe::test::require(template_text.find("MOEX_TWIME_TEST_CREDENTIALS") != std::string::npos,
                                       "tracked live-session template must use local credential placeholders");
        moex::twime_sbe::test::require(template_text.find("198.51.100.") == std::string::npos,
                                       "tracked live-session template must not contain a real external host");

        const auto example_text = read_text(root / "profiles" / "test_twime_live_session_local.example.yaml");
        moex::twime_sbe::test::require(example_text.find("host: localhost") != std::string::npos,
                                       "live-session example must remain localhost-only");
        moex::twime_sbe::test::require(example_text.find("test_session_armed_required: true") != std::string::npos,
                                       "live-session example must require explicit test-session arming");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
