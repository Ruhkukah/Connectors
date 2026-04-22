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
        const auto fixture_root = make_temp_directory("plaza2_scheme_drift_test");
        const auto cleanup = [&]() { remove_tree(fixture_root); };

        auto scheme_text = build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test");
        const auto removed_field = std::string("field=private_order_id,i8\n");
        const auto removed_pos = scheme_text.find(removed_field);
        require(removed_pos != std::string::npos, "expected runtime scheme fixture field missing");
        scheme_text.erase(removed_pos, removed_field.size());
        scheme_text += "[table:EXTRA:unexpected_table]\nfield=replID,i8\nfield=replRev,i8\n";

        const auto fixture =
            materialize_runtime_fixture(fixture_root, fake_library, Plaza2Environment::Test, scheme_text);

        Plaza2Settings settings;
        settings.environment = Plaza2Environment::Test;
        settings.runtime_root = fixture.root;
        settings.expected_spectra_release = "SPECTRA95";
        settings.expected_scheme_sha256 = std::string(64, '0');
        const auto report = Plaza2RuntimeProbe::probe(settings);
        require(report.compatibility == Plaza2Compatibility::Incompatible, "drifted runtime must be incompatible");

        bool found_hash = false;
        bool found_version = false;
        bool found_signature_drift = false;
        bool found_unexpected_table = false;
        for (const auto& issue : report.issues) {
            found_hash = found_hash || issue.code == Plaza2ProbeIssueCode::FileHashMismatch;
            found_version = found_version || issue.code == Plaza2ProbeIssueCode::UnsupportedVersion;
            found_signature_drift = found_signature_drift ||
                                    issue.code == Plaza2ProbeIssueCode::ReviewedTableSignatureMismatch ||
                                    issue.code == Plaza2ProbeIssueCode::ReviewedTableMissing;
            found_unexpected_table =
                found_unexpected_table || issue.code == Plaza2ProbeIssueCode::RuntimeTableUnexpected;
        }

        require(found_hash, "scheme hash mismatch should be reported");
        require(found_version, "spectra release mismatch should be reported");
        require(found_signature_drift, "reviewed-vs-runtime signature drift should be reported");
        require(found_unexpected_table, "unexpected runtime table should be reported");

        write_text_file(fixture.scheme_path, "[broken\n");
        const auto broken_report = Plaza2RuntimeProbe::probe(settings);
        bool found_parse_error = false;
        for (const auto& issue : broken_report.issues) {
            if (issue.code == Plaza2ProbeIssueCode::RuntimeSchemeParseFailed) {
                found_parse_error = true;
            }
        }
        require(found_parse_error, "malformed runtime scheme should be reported");

        cleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
