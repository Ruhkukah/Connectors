#pragma once

#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace moex::plaza2::test {

struct RuntimeFixturePaths {
    std::filesystem::path root;
    std::filesystem::path library_path;
    std::filesystem::path scheme_dir;
    std::filesystem::path scheme_path;
    std::filesystem::path config_dir;
};

[[nodiscard]] std::filesystem::path make_temp_directory(std::string_view prefix);
void remove_tree(const std::filesystem::path& path);
void require(bool condition, std::string_view message);
void write_text_file(const std::filesystem::path& path, std::string_view text);

[[nodiscard]] std::string build_vendor_like_runtime_scheme(std::string_view spectra_release,
                                                           std::string_view dds_version,
                                                           std::string_view target_polygon);
[[nodiscard]] RuntimeFixturePaths materialize_runtime_fixture(const std::filesystem::path& root,
                                                              const std::filesystem::path& fake_library,
                                                              moex::plaza2::cgate::Plaza2Environment environment,
                                                              std::string_view scheme_text);

} // namespace moex::plaza2::test
