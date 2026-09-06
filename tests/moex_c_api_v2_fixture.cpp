#include "plaza2_runtime_test_support.hpp"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    try {
        moex::plaza2::test::require(argc == 3, "fake runtime path and output root");
        const auto root = std::filesystem::path(argv[2]);
        moex::plaza2::test::remove_tree(root);
        (void)moex::plaza2::test::materialize_runtime_fixture(
            root, argv[1], moex::plaza2::cgate::Plaza2Environment::Test,
            moex::plaza2::test::build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test"));
        std::cout << root.string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
