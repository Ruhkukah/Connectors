#include "moex/twime_sbe/twime_schema.hpp"

#include "twime_test_support.hpp"

#include <fstream>
#include <iostream>

int main() {
    try {
        const auto& schema = moex::twime_sbe::TwimeSchemaView::info();
        moex::twime_sbe::test::require(schema.schema_id == 19781, "unexpected schema id");
        moex::twime_sbe::test::require(schema.schema_version == 7, "unexpected schema version");
        moex::twime_sbe::test::require(schema.byte_order == "littleEndian", "unexpected byte order");
        moex::twime_sbe::test::require(schema.package == "moex_spectra_twime", "unexpected package");

        const auto manifest_text = moex::twime_sbe::test::read_text_file(
            moex::twime_sbe::test::project_root() / "protocols/twime_sbe/schema/schema.manifest.json");
        moex::twime_sbe::test::require(manifest_text.find(std::string(schema.sha256)) != std::string::npos,
                                       "schema hash missing from schema.manifest.json");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
