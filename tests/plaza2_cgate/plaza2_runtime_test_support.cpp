#include "plaza2_runtime_test_support.hpp"

#include "plaza2_generated_metadata.hpp"

#include <cstdlib>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace moex::plaza2::test {

namespace {

struct ReviewedFieldSection {
    std::string stream_name;
    std::string table_name;
    std::string field_name;
    std::string type_token;
};

std::string platform_library_name() {
#if defined(__APPLE__)
    return "libcgate.dylib";
#else
    return "libcgate.so";
#endif
}

std::string trim_copy(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::map<std::pair<std::string, std::string>, std::vector<std::pair<std::string, std::string>>>
parse_reviewed_fields(const std::filesystem::path& reviewed_ini_path) {
    std::ifstream input(reviewed_ini_path);
    if (!input) {
        throw std::runtime_error("failed to open reviewed PLAZA II fixture: " + reviewed_ini_path.string());
    }

    std::map<std::pair<std::string, std::string>, std::vector<std::pair<std::string, std::string>>> grouped_fields;
    std::optional<ReviewedFieldSection> current_field;
    std::string line;

    const auto flush_field = [&]() {
        if (!current_field.has_value()) {
            return;
        }
        if (current_field->stream_name.empty() || current_field->table_name.empty() ||
            current_field->field_name.empty() || current_field->type_token.empty()) {
            throw std::runtime_error("reviewed PLAZA II fixture contains an incomplete field section");
        }
        grouped_fields[{current_field->stream_name, current_field->table_name}].push_back(
            {current_field->field_name, current_field->type_token});
        current_field.reset();
    };

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
            continue;
        }
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            flush_field();
            const auto section = trimmed.substr(1, trimmed.size() - 2);
            if (section.rfind("field:", 0) == 0) {
                current_field = ReviewedFieldSection{};
            }
            continue;
        }
        if (!current_field.has_value()) {
            continue;
        }

        const auto equals = trimmed.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const auto key = trim_copy(trimmed.substr(0, equals));
        const auto value = trim_copy(trimmed.substr(equals + 1));
        if (key == "stream_name") {
            current_field->stream_name = value;
        } else if (key == "table_name") {
            current_field->table_name = value;
        } else if (key == "field_name") {
            current_field->field_name = value;
        } else if (key == "type_token") {
            current_field->type_token = value;
        }
    }
    flush_field();
    return grouped_fields;
}

} // namespace

void write_text_file(const std::filesystem::path& path, std::string_view text) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path);
    output << text;
}

std::filesystem::path make_temp_directory(std::string_view prefix) {
    const auto path = std::filesystem::temp_directory_path() /
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

std::string build_vendor_like_runtime_scheme(std::string_view spectra_release, std::string_view dds_version,
                                             std::string_view target_polygon) {
    using namespace moex::plaza2::generated;

    const auto reviewed_ini_path =
        std::filesystem::path(MOEX_SOURCE_ROOT) / "protocols" / "plaza2_cgate" / "schema" / "plaza2_forts_reviewed.ini";
    const auto reviewed_fields = parse_reviewed_fields(reviewed_ini_path);

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
            const auto grouped_it =
                reviewed_fields.find({std::string(stream.stream_name), std::string(table.table_name)});
            if (grouped_it == reviewed_fields.end()) {
                throw std::runtime_error("reviewed PLAZA II fixture is missing table fields for " +
                                         std::string(stream.stream_name) + "." + std::string(table.table_name));
            }
            if (grouped_it->second.size() != FieldsForTable(table.table_code).size()) {
                throw std::runtime_error("reviewed PLAZA II fixture field count drifted for " +
                                         std::string(stream.stream_name) + "." + std::string(table.table_name));
            }
            out << "[table:" << stream.stream_name << ':' << table.table_name << "]\n";
            for (const auto& field : grouped_it->second) {
                out << "field=" << field.first << ',' << field.second << '\n';
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
