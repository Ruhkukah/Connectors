#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <array>
#include <iostream>

namespace {

using moex::plaza2::cgate::Plaza2DecodedFieldValue;
using moex::plaza2::cgate::Plaza2DecodedValueKind;
using moex::plaza2::generated::FieldCode;

Plaza2DecodedFieldValue signed_field(FieldCode code, std::int64_t value) {
    return {
        .field_code = code,
        .kind = Plaza2DecodedValueKind::SignedInteger,
        .signed_value = value,
    };
}

Plaza2DecodedFieldValue unsigned_field(FieldCode code, std::uint64_t value) {
    return {
        .field_code = code,
        .kind = Plaza2DecodedValueKind::UnsignedInteger,
        .unsigned_value = value,
    };
}

Plaza2DecodedFieldValue decimal_field(FieldCode code, std::string_view value) {
    return {
        .field_code = code,
        .kind = Plaza2DecodedValueKind::Decimal,
        .text_value = value,
    };
}

} // namespace

int main() {
    try {
        using namespace moex::plaza2::cgate;
        using moex::plaza2::test::require;

        Plaza2Aggr20BookProjector projector;
        projector.begin_transaction();
        const std::array bid = {
            unsigned_field(FieldCode::kFortsAggrReplOrdersAggrReplId, 1),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrReplRev, 11),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrIsinId, 1001),
            decimal_field(FieldCode::kFortsAggrReplOrdersAggrPrice, "100.50"),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrVolume, 7),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrDir, 1),
            unsigned_field(FieldCode::kFortsAggrReplOrdersAggrMomentNs, 42),
        };
        require(!projector.on_row(bid), "bid AGGR20 row should be accepted while transaction is open");
        require(projector.snapshot().row_count == 0, "AGGR20 row must not be visible before TN_COMMIT");

        const std::array ask = {
            unsigned_field(FieldCode::kFortsAggrReplOrdersAggrReplId, 2),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrReplRev, 12),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrIsinId, 1001),
            decimal_field(FieldCode::kFortsAggrReplOrdersAggrPrice, "101.25"),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrVolume, 5),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrDir, 2),
            unsigned_field(FieldCode::kFortsAggrReplOrdersAggrMomentNs, 43),
        };
        require(!projector.on_row(ask), "ask AGGR20 row should be accepted while transaction is open");
        require(!projector.commit(), "AGGR20 commit should succeed");

        const auto& snapshot = projector.snapshot();
        require(snapshot.row_count == 2, "AGGR20 snapshot row count mismatch");
        require(snapshot.instrument_count == 1, "AGGR20 instrument count mismatch");
        require(snapshot.bid_depth_levels == 1, "AGGR20 bid depth mismatch");
        require(snapshot.ask_depth_levels == 1, "AGGR20 ask depth mismatch");
        require(snapshot.top_bid.has_value() && snapshot.top_bid->price == "100.50", "AGGR20 top bid mismatch");
        require(snapshot.top_ask.has_value() && snapshot.top_ask->price == "101.25", "AGGR20 top ask mismatch");
        require(snapshot.last_repl_id == 2, "AGGR20 last replID mismatch");
        require(snapshot.last_repl_rev == 12, "AGGR20 last replRev mismatch");

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
