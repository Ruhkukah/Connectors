#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"

#include "plaza2_runtime_test_support.hpp"

#include <array>
#include <chrono>
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

        auto local_now = Plaza2Aggr20BookProjector::Clock::time_point{} + std::chrono::seconds(7);
        Plaza2Aggr20BookProjector projector([&local_now] { return local_now; });
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
        const std::array other_bid = {
            unsigned_field(FieldCode::kFortsAggrReplOrdersAggrReplId, 3),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrReplRev, 13),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrIsinId, 2002),
            decimal_field(FieldCode::kFortsAggrReplOrdersAggrPrice, "10000.00"),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrVolume, 3),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrDir, 1),
            unsigned_field(FieldCode::kFortsAggrReplOrdersAggrMoment, 100),
        };
        const std::array other_ask = {
            unsigned_field(FieldCode::kFortsAggrReplOrdersAggrReplId, 4),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrReplRev, 14),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrIsinId, 2002),
            decimal_field(FieldCode::kFortsAggrReplOrdersAggrPrice, "10001.00"),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrVolume, 3),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrDir, 2),
            unsigned_field(FieldCode::kFortsAggrReplOrdersAggrMoment, 101),
        };
        require(!projector.on_row(other_bid), "second-instrument bid should be accepted");
        require(!projector.on_row(other_ask), "second-instrument ask should be accepted");
        require(!projector.commit(), "AGGR20 commit should succeed");

        const auto& snapshot = projector.snapshot();
        require(snapshot.row_count == 4, "AGGR20 snapshot row count mismatch");
        require(snapshot.instrument_count == 2, "AGGR20 instrument count mismatch");
        require(snapshot.top_bid.has_value() && snapshot.top_bid->price == "10000.00",
                "global AGGR20 diagnostic top bid mismatch");
        require(snapshot.top_ask.has_value() && snapshot.top_ask->price == "101.25",
                "global AGGR20 diagnostic top ask mismatch");
        require(snapshot.last_repl_id == 4, "AGGR20 last replID mismatch");
        require(snapshot.last_repl_rev == 14, "AGGR20 last replRev mismatch");
        const auto target = projector.snapshot_for_isin(1001);
        require(target.has_value() && target->top_bid.has_value() && target->top_ask.has_value(),
                "target instrument should have a two-sided scoped snapshot");
        require(target->top_bid->price == "100.50" && target->top_ask->price == "101.25",
                "instrument-scoped BBO must not use another instrument");
        require(target->committed_at == local_now, "scoped snapshot must carry local monotonic commit time");
        require(!projector.snapshot_for_isin(9999).has_value(), "absent instrument must have no scoped snapshot");

        local_now += std::chrono::seconds(1);
        projector.begin_transaction();
        const std::array delete_target_ask = {
            unsigned_field(FieldCode::kFortsAggrReplOrdersAggrReplId, 5),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrReplRev, 15),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrIsinId, 1001),
            decimal_field(FieldCode::kFortsAggrReplOrdersAggrPrice, "101.25"),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrVolume, 0),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrDir, 2),
        };
        require(!projector.on_row(delete_target_ask), "target deletion should be accepted");
        require(!projector.commit(), "target deletion commit should succeed");
        const auto one_sided = projector.snapshot_for_isin(1001);
        require(one_sided.has_value() && one_sided->top_bid.has_value() && !one_sided->top_ask.has_value(),
                "deleted target ask must leave a one-sided scoped snapshot");
        require(one_sided->committed_at == local_now, "scoped timestamp must advance on every commit");

        local_now += std::chrono::seconds(1);
        projector.begin_transaction();
        const std::array other_update = {
            unsigned_field(FieldCode::kFortsAggrReplOrdersAggrReplId, 6),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrReplRev, 16),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrIsinId, 2002),
            decimal_field(FieldCode::kFortsAggrReplOrdersAggrPrice, "9999.00"),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrVolume, 4),
            signed_field(FieldCode::kFortsAggrReplOrdersAggrDir, 1),
        };
        require(!projector.on_row(other_update), "other instrument update should be accepted");
        require(!projector.commit(), "other instrument update should commit");
        const auto target_after_other_update = projector.snapshot_for_isin(1001);
        require(target_after_other_update.has_value() && target_after_other_update->top_bid->price == "100.50",
                "updating another instrument must not change target BBO");

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
