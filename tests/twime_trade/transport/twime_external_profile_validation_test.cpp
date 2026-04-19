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
        const auto template_text = read_text(root / "profiles" / "test_twime_tcp_external.template.yaml");
        moex::twime_sbe::test::require(template_text.find("TEST_ENDPOINT_HOST_PLACEHOLDER") != std::string::npos,
                                       "tracked external test template must keep a placeholder host");
        moex::twime_sbe::test::require(template_text.find("MOEX_TWIME_TEST_CREDENTIALS") != std::string::npos,
                                       "tracked external test template must use env credential placeholders");
        moex::twime_sbe::test::require(template_text.find("198.51.100.10") == std::string::npos,
                                       "tracked external test template must not contain a real non-loopback host");

        const auto local_example_text = read_text(root / "profiles" / "test_twime_tcp_external_local.example.yaml");
        moex::twime_sbe::test::require(local_example_text.find("host: localhost") != std::string::npos,
                                       "local external example must remain localhost-only by default");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    return 0;
}
