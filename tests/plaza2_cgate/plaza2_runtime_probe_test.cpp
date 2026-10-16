#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
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

        const auto hash_input = fixture_root / "hash-input.bin";
        require(plaza2_sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "SHA-256 known vector mismatch");
        for (const auto size : {std::size_t(0), std::size_t(3), std::size_t(2 * 1024 * 1024 + 37)}) {
            std::string data(size, '\0');
            for (std::size_t i = 0; i < size; ++i)
                data[i] = static_cast<char>((i * 131 + 17) % 256);
            {
                std::ofstream file(hash_input, std::ios::binary);
                file.write(data.data(), static_cast<std::streamsize>(data.size()));
                require(bool(file), "SHA-256 fixture write failed");
            }
            require(plaza2_sha256_file(hash_input) == plaza2_sha256_hex(data),
                    "streaming file SHA-256 differs across chunk/tail boundaries");
        }
        bool refused{};
        try {
            (void)plaza2_sha256_file(fixture_root / "missing-hash-input");
        } catch (const std::runtime_error&) {
            refused = true;
        }
        require(refused, "missing file was fingerprinted as a valid empty file");

        Plaza2Settings settings;
        settings.environment = Plaza2Environment::Test;
        settings.runtime_root = fixture.root;
        settings.expected_spectra_release = "SPECTRA93";
        const auto report = Plaza2RuntimeProbe::probe(settings);
        require(report.compatibility == Plaza2Compatibility::Compatible, "expected compatible runtime probe result");
        require(report.runtime_root_present, "runtime root should be present");
        require(report.runtime_library_present && report.runtime_library_loadable, "fake runtime library should load");
        require(report.runtime_library_sha256.size() == 64, "runtime library SHA-256 fingerprint should be recorded");
        require(report.runtime_identity_recognized && report.runtime_version == "fake-runtime-v1",
                "offline fake runtime identity should be explicit");
        require(report.scheme_file_present, "runtime scheme file should be detected");
        require(report.config_dir_present, "config directory should be detected");
        require(report.layout.version_markers.spectra_release == "SPECTRA93", "spectra release marker mismatch");
        require(report.layout.version_markers.dds_version == "93.0.0.0", "dds version marker mismatch");
        require(report.layout.version_markers.target_polygon == "test", "target polygon marker mismatch");
        for (const auto required_symbol : Plaza2RuntimeProbe::required_runtime_symbols()) {
            require(std::ranges::find(report.resolved_symbols, required_symbol) != report.resolved_symbols.end(),
                    "required runtime symbol was not resolved: " + std::string(required_symbol));
        }
        require(report.trading_capable && report.missing_trading_symbols.empty(),
                "fake runtime should expose the complete publisher/reply trading capability");
        for (const auto required_symbol : Plaza2RuntimeProbe::required_trading_symbols()) {
            require(std::ranges::find(report.resolved_symbols, required_symbol) != report.resolved_symbols.end(),
                    "required trading symbol was not resolved: " + std::string(required_symbol));
        }
        require(std::ranges::find(Plaza2RuntimeProbe::required_runtime_symbols(), "cg_pub_post") ==
                    Plaza2RuntimeProbe::required_runtime_symbols().end(),
                "publisher symbols must remain optional for read-only runtime compatibility");
        require(std::ranges::find(Plaza2RuntimeProbe::required_trading_symbols(), "cg_pub_post") !=
                    Plaza2RuntimeProbe::required_trading_symbols().end(),
                "trading capability must explicitly require publisher symbols");

        Plaza2Settings exact_hash_settings = settings;
        exact_hash_settings.expected_runtime_library_sha256 = report.runtime_library_sha256;
        const auto exact_hash_report = Plaza2RuntimeProbe::probe(exact_hash_settings);
        require(exact_hash_report.compatibility == Plaza2Compatibility::Compatible,
                "exact runtime library hash should be accepted");

        Plaza2Settings incorrect_hash_settings = settings;
        incorrect_hash_settings.expected_runtime_library_sha256 = std::string(64, '0');
        const auto incorrect_hash_report = Plaza2RuntimeProbe::probe(incorrect_hash_settings);
        require(incorrect_hash_report.compatibility == Plaza2Compatibility::CompatibleWithWarnings,
                "runtime hash drift must remain diagnostic");
        require(std::ranges::any_of(incorrect_hash_report.issues,
                                    [](const auto& issue) {
                                        return issue.code == Plaza2ProbeIssueCode::FileHashMismatch && !issue.fatal;
                                    }),
                "runtime library hash drift should report a warning");

        Plaza2Settings changed_release_settings = settings;
        changed_release_settings.expected_spectra_release = "SPECTRA9.9.0";
        const auto changed_release_report = Plaza2RuntimeProbe::probe(changed_release_settings);
        require(changed_release_report.compatibility == Plaza2Compatibility::CompatibleWithWarnings,
                "Spectra release drift must remain diagnostic");
        require(changed_release_report.layout.version_markers.spectra_release == "SPECTRA93" &&
                    changed_release_report.runtime_library_loadable && changed_release_report.trading_capable,
                "Spectra warning must preserve the observed version and runtime capabilities");
        require(std::ranges::any_of(changed_release_report.issues,
                                    [](const auto& issue) {
                                        return issue.code == Plaza2ProbeIssueCode::UnsupportedVersion && !issue.fatal &&
                                               issue.subject == "scheme";
                                    }),
                "Spectra release drift must report a nonfatal UnsupportedVersion issue");

        Plaza2Settings scoped_config_settings = settings;
        scoped_config_settings.env_open_settings = "ini=config/t1.ini;key=${MOEX_PLAZA2_TEST_CREDENTIALS}";
        std::filesystem::remove(fixture.config_dir / "router.ini");
        const auto scoped_config_report = Plaza2RuntimeProbe::probe(scoped_config_settings);
        require(scoped_config_report.compatibility == Plaza2Compatibility::CompatibleWithWarnings,
                "configured TEST route should not fail on unrelated config files");

        const auto missing_config = Plaza2RuntimeProbe::probe(settings);
        require(missing_config.compatibility == Plaza2Compatibility::Incompatible,
                "missing config file should mark runtime incompatible");

        std::filesystem::remove(fixture.config_dir / "t1.ini");
        const auto missing_scoped_config = Plaza2RuntimeProbe::probe(scoped_config_settings);
        require(missing_scoped_config.compatibility == Plaza2Compatibility::Incompatible,
                "missing configured TEST ini should mark runtime incompatible");

        write_text_file(fixture.config_dir / "rogue.ini", "[rogue]\n");
        write_text_file(fixture.config_dir / "router.ini", "[router]\n");
        write_text_file(fixture.config_dir / "t1.ini", "[t1]\n");
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
