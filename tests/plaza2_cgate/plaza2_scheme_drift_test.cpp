#include "moex/plaza2/cgate/plaza2_runtime.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::string remove_field_from_table(std::string scheme_text, std::string_view table_header,
                                    std::string_view field_line) {
    const auto table_pos = scheme_text.find(table_header);
    moex::plaza2::test::require(table_pos != std::string::npos, "expected runtime scheme fixture table missing");
    const auto next_table_pos = scheme_text.find("\n[table:", table_pos + table_header.size());
    const auto search_end = next_table_pos == std::string::npos ? scheme_text.size() : next_table_pos;
    const auto field_pos = scheme_text.find(field_line, table_pos);
    moex::plaza2::test::require(field_pos != std::string::npos && field_pos < search_end,
                                "expected runtime scheme fixture field missing");
    scheme_text.erase(field_pos, field_line.size());
    return scheme_text;
}

} // namespace

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

        const auto baseline_scheme_text = build_vendor_like_runtime_scheme("SPECTRA93", "93.0.0.0", "test");
        const auto compatible_fixture = materialize_runtime_fixture(fixture_root / "compatible", fake_library,
                                                                    Plaza2Environment::Test, baseline_scheme_text);

        Plaza2Settings compatible_settings;
        compatible_settings.environment = Plaza2Environment::Test;
        compatible_settings.runtime_root = compatible_fixture.root;
        compatible_settings.expected_spectra_release = "SPECTRA93";
        const auto compatible_report = Plaza2RuntimeProbe::probe(compatible_settings);
        require(compatible_report.compatibility == Plaza2Compatibility::Compatible,
                "unchanged runtime fixture must be compatible");
        require(compatible_report.scheme_drift.compatibility == Plaza2Compatibility::Compatible,
                "unchanged scheme must be compatible");

        const auto warning_scheme_text = remove_field_from_table(
            baseline_scheme_text, "[table:FORTS_REFDATA_REPL:clearing_members]\n", "field=code,c2\n");
        const auto warning_fixture = materialize_runtime_fixture(fixture_root / "warning", fake_library,
                                                                 Plaza2Environment::Test, warning_scheme_text);
        Plaza2Settings warning_settings;
        warning_settings.environment = Plaza2Environment::Test;
        warning_settings.runtime_root = warning_fixture.root;
        warning_settings.expected_spectra_release = "SPECTRA93";
        const auto warning_report = Plaza2RuntimeProbe::probe(warning_settings);
        require(warning_report.compatibility == Plaza2Compatibility::CompatibleWithWarnings,
                "non-projected clearing_members drift must be compatible with warnings");
        require(warning_report.scheme_drift.compatibility == Plaza2Compatibility::CompatibleWithWarnings,
                "clearing_members drift must not be fatal");
        require(warning_report.scheme_drift.warning_drift_count > 0, "warning drift count should be visible");
        require(warning_report.scheme_drift.fatal_drift_count == 0, "warning-only drift must not count as fatal");
        bool found_clearing_members_warning = false;
        for (const auto& table : warning_report.scheme_drift.warning_drift_tables) {
            found_clearing_members_warning =
                found_clearing_members_warning || table == "FORTS_REFDATA_REPL.clearing_members";
        }
        require(found_clearing_members_warning, "clearing_members warning table should be reported");

        auto fatal_scheme_text = remove_field_from_table(baseline_scheme_text, "[table:FORTS_TRADE_REPL:orders_log]\n",
                                                         "field=private_order_id,i8\n");
        fatal_scheme_text += "[table:EXTRA:unexpected_table]\nfield=replID,i8\nfield=replRev,i8\n";
        const auto fatal_fixture = materialize_runtime_fixture(fixture_root / "fatal", fake_library,
                                                               Plaza2Environment::Test, fatal_scheme_text);

        Plaza2Settings fatal_settings;
        fatal_settings.environment = Plaza2Environment::Test;
        fatal_settings.runtime_root = fatal_fixture.root;
        fatal_settings.expected_spectra_release = "SPECTRA95";
        fatal_settings.expected_scheme_sha256 = std::string(64, '0');
        const auto report = Plaza2RuntimeProbe::probe(fatal_settings);
        require(report.compatibility == Plaza2Compatibility::Incompatible, "required-table drift must be fatal");

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
        require(report.scheme_drift.fatal_drift_count > 0, "fatal drift count should be visible");

        write_text_file(fatal_fixture.scheme_path, "[broken\n");
        const auto broken_report = Plaza2RuntimeProbe::probe(fatal_settings);
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
