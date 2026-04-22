#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "expected fake runtime library path\n";
            return 1;
        }

        using namespace moex::plaza2::cgate;
        using namespace moex::plaza2::test;

        const auto fake_library = std::filesystem::path(argv[1]);
        const auto fixture_root = make_temp_directory("plaza2_runtime_probe_test");
        const auto cleanup = [&]() { remove_tree(fixture_root); };

        const auto scheme_text = build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test");
        const auto fixture =
            materialize_runtime_fixture(fixture_root, fake_library, Plaza2Environment::Test, scheme_text);

        Plaza2Settings settings;
        settings.environment = Plaza2Environment::Test;
        settings.runtime_root = fixture.root;
        settings.expected_spectra_release = "SPECTRA93";
        const auto report = Plaza2RuntimeProbe::probe(settings);
        require(report.compatibility == Plaza2Compatibility::Compatible, "expected compatible runtime probe result");
        require(report.runtime_root_present, "runtime root should be present");
        require(report.runtime_library_present && report.runtime_library_loadable, "fake runtime library should load");
        require(report.scheme_file_present, "runtime scheme file should be detected");
        require(report.config_dir_present, "config directory should be detected");
        require(report.layout.version_markers.spectra_release == "SPECTRA93", "spectra release marker mismatch");
        require(report.layout.version_markers.dds_version == "93.0.0.0", "dds version marker mismatch");
        require(report.layout.version_markers.target_polygon == "test", "target polygon marker mismatch");
        require(report.resolved_symbols.size() == Plaza2RuntimeProbe::required_runtime_symbols().size(),
                "not all runtime symbols were resolved");

        std::filesystem::remove(fixture.config_dir / "router.ini");
        const auto missing_config = Plaza2RuntimeProbe::probe(settings);
        require(missing_config.compatibility == Plaza2Compatibility::Incompatible,
                "missing config file should mark runtime incompatible");

        write_text_file(fixture.config_dir / "rogue.ini", "[rogue]\n");
        write_text_file(fixture.config_dir / "router.ini", "[router]\n");
        const auto unexpected_config = Plaza2RuntimeProbe::probe(settings);
        bool found_unexpected = false;
        for (const auto& issue : unexpected_config.issues) {
            if (issue.code == Plaza2ProbeIssueCode::UnexpectedConfigFile && issue.subject == "rogue.ini") {
                found_unexpected = true;
            }
        }
        require(found_unexpected, "unexpected config file should be reported");

        cleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
