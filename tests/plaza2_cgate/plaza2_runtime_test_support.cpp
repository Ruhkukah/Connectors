#include "plaza2_runtime_test_support.hpp"

#include "plaza2_generated_metadata.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace moex::plaza2::test {

namespace {

std::string platform_library_name() {
#if defined(__APPLE__)
    return "libcgate.dylib";
#else
    return "libcgate.so";
#endif
}

void write_text_file(const std::filesystem::path& path, std::string_view text) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path);
    output << text;
}

} // namespace

std::filesystem::path make_temp_directory(std::string_view prefix) {
    const auto path =
        std::filesystem::temp_directory_path() /
        (std::string(prefix) + "_" + std::to_string(static_cast<unsigned long long>(::getpid())));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void remove_tree(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string build_vendor_like_runtime_scheme(std::string_view spectra_release,
                                             std::string_view dds_version,
                                             std::string_view target_polygon) {
    using namespace moex::plaza2::generated;

    std::ostringstream out;
    out << "; Spectra release: " << spectra_release << '\n';
    out << "; DDS version: " << dds_version << '\n';
    out << "; Target poligon: " << target_polygon << '\n';
    out << ";\n";

    for (const auto& stream : StreamDescriptors()) {
        out << "[dbscheme:" << stream.stream_name << "]\n";
        for (const auto& table : TablesForStream(stream.stream_code)) {
            out << "table=" << table.table_name << '\n';
        }
        out << '\n';

        for (const auto& table : TablesForStream(stream.stream_code)) {
            out << "[table:" << stream.stream_name << ':' << table.table_name << "]\n";
            for (const auto& field : FieldsForTable(table.table_code)) {
                out << "field=" << field.field_name << ',' << field.type_token << '\n';
            }
            out << '\n';
        }
    }

    return out.str();
}

RuntimeFixturePaths materialize_runtime_fixture(const std::filesystem::path& root,
                                                const std::filesystem::path& fake_library,
                                                moex::plaza2::cgate::Plaza2Environment environment,
                                                std::string_view scheme_text) {
    RuntimeFixturePaths fixture;
    fixture.root = root;
    fixture.scheme_dir = root / "scheme";
    fixture.config_dir = root / "config";
    std::filesystem::create_directories(root / "bin");
    std::filesystem::create_directories(fixture.scheme_dir);
    std::filesystem::create_directories(fixture.config_dir);

    fixture.library_path = root / "bin" / platform_library_name();
    std::filesystem::copy_file(fake_library, fixture.library_path, std::filesystem::copy_options::overwrite_existing);

    fixture.scheme_path = fixture.scheme_dir / "forts_scheme.ini";
    write_text_file(fixture.scheme_path, scheme_text);

    for (const auto name : moex::plaza2::cgate::Plaza2RuntimeProbe::expected_config_filenames(environment)) {
        write_text_file(fixture.config_dir / name, "[placeholder]\n");
    }

    return fixture;
}

} // namespace moex::plaza2::test
