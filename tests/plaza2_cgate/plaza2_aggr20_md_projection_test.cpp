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
        Plaza2Aggr20BookProjector strict_projector;
        strict_projector.begin_transaction();
        auto malformed_bid = bid;
        malformed_bid[3].text_value = "100.5000001";
        const auto malformed_error = strict_projector.on_row(malformed_bid);
        require(malformed_error && malformed_error.code == Plaza2ErrorCode::DecodeFailed,
                "AGGR20 malformed price must be surfaced as a decode failure");
        require(kPlaza2Aggr20FractionalDigits == 5 && kPlaza2Aggr20PriceScale == 100'000,
                "AGGR20 must use the generated d16.5 scale-5 contract");
        require(parse_fixed_point("100.50000", kPlaza2Aggr20FractionalDigits, true, kPlaza2D16_5DecimalPrecision) ==
                    10'050'000,
                "AGGR20 d16.5 must preserve scale-5 units");
        require(parse_fixed_point("0", kPlaza2Aggr20FractionalDigits, true, kPlaza2D16_5DecimalPrecision) == 0 &&
                    parse_fixed_point("0.00000", kPlaza2Aggr20FractionalDigits, true, kPlaza2D16_5DecimalPrecision) ==
                        0,
                "AGGR20 d16.5 must accept zero spellings");
        require(
            parse_fixed_point("-0.00001", kPlaza2Aggr20FractionalDigits, true, kPlaza2D16_5DecimalPrecision) == -1 &&
                parse_fixed_point("-100.50000", kPlaza2Aggr20FractionalDigits, true, kPlaza2D16_5DecimalPrecision) ==
                    -10'050'000,
            "AGGR20 d16.5 must accept signed values");
        require(parse_fixed_point("99999999999.99999", kPlaza2Aggr20FractionalDigits, true,
                                  kPlaza2D16_5DecimalPrecision) == 9'999'999'999'999'999LL &&
                    parse_fixed_point("-99999999999.99999", kPlaza2Aggr20FractionalDigits, true,
                                      kPlaza2D16_5DecimalPrecision) == -9'999'999'999'999'999LL,
                "AGGR20 d16.5 must accept both valid domain extremes");
        require(!parse_fixed_point("12.345678", kPlaza2Aggr20FractionalDigits, true, kPlaza2D16_5DecimalPrecision)
                     .has_value(),
                "AGGR20 fixed-point parser must reject six fractional digits");
        require(!parse_fixed_point("12x34", kPlaza2Aggr20FractionalDigits, false).has_value(),
                "AGGR20 fixed-point parser must reject non-numeric characters");
        require(
            !parse_fixed_point("100000000000.00000", kPlaza2Aggr20FractionalDigits, true, kPlaza2D16_5DecimalPrecision)
                 .has_value(),
            "AGGR20 d16.5 parser must reject precision overflow");
        require(!parse_fixed_point("9223372036854775808", 0, false).has_value(),
                "fixed-point parser must reject signed overflow");
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
        require(snapshot.top_bid.has_value() && snapshot.top_bid->price == "10000.00" &&
                    snapshot.top_bid->price_scaled == 1'000'000'000,
                "global AGGR20 diagnostic top bid mismatch");
        require(snapshot.top_ask.has_value() && snapshot.top_ask->price == "101.25" &&
                    snapshot.top_ask->price_scaled == 10'125'000,
                "global AGGR20 diagnostic top ask mismatch");
        require(snapshot.last_repl_id == 4, "AGGR20 last replID mismatch");
        require(snapshot.last_repl_rev == 14, "AGGR20 last replRev mismatch");
        const auto target = projector.snapshot_for_isin(1001);
        require(target.has_value() && target->top_bid.has_value() && target->top_ask.has_value(),
                "target instrument should have a two-sided scoped snapshot");
        require(target->top_bid->price == "100.50" && target->top_ask->price == "101.25",
                "instrument-scoped BBO must not use another instrument");
        require(target->top_bid->price_scaled == 10'050'000 && target->top_ask->price_scaled == 10'125'000,
                "instrument-scoped d16.5 units must use scale 100000");
        require(target->committed_at == local_now, "scoped snapshot must carry local monotonic commit time");
        require(target->source_snapshot_version != 0 && target->source_snapshot_hash != 0,
                "target snapshot must expose a version and deterministic source hash");
        require(target->levels.size() == 2 && target->levels[0].dir == 1 && target->levels[1].dir == 2,
                "target levels must be sorted bid-descending then ask-ascending");
        const auto initial_target_version = target->source_snapshot_version;
        const auto initial_target_hash = target->source_snapshot_hash;
        require(!projector.snapshot_for_isin(9999).has_value(), "absent instrument must have no scoped snapshot");

        local_now += std::chrono::seconds(1);
        projector.begin_transaction();
        const std::array delete_target_ask = {
            unsigned_field(FieldCode::kFortsAggrReplOrdersAggrReplId, 2),
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
        require(one_sided->last_repl_id == 2 && one_sided->last_repl_rev == 15,
                "scoped deletion must retain the target's latest replication identity");
        require(one_sided->source_snapshot_version > initial_target_version &&
                    one_sided->source_snapshot_hash != initial_target_hash,
                "target value changes must advance version and hash");

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
        require(target_after_other_update->committed_at == local_now - std::chrono::seconds(1),
                "updating another instrument must not refresh target local freshness");
        require(target_after_other_update->source_snapshot_version == one_sided->source_snapshot_version &&
                    target_after_other_update->source_snapshot_hash == one_sided->source_snapshot_hash,
                "unrelated ISIN updates must not refresh target source version or hash");

        // Replication slots can move price/side without a zero-volume old-price row.
        Plaza2Aggr20BookProjector slots;
        const auto stage = [&](std::uint64_t id, std::int64_t isin, std::int64_t dir, std::string_view price,
                               std::int64_t volume, std::int64_t act = 0) {
            const std::array row = {
                unsigned_field(FieldCode::kFortsAggrReplOrdersAggrReplId, id),
                signed_field(FieldCode::kFortsAggrReplOrdersAggrReplRev, 100),
                signed_field(FieldCode::kFortsAggrReplOrdersAggrReplAct, act),
                signed_field(FieldCode::kFortsAggrReplOrdersAggrIsinId, isin),
                signed_field(FieldCode::kFortsAggrReplOrdersAggrDir, dir),
                decimal_field(FieldCode::kFortsAggrReplOrdersAggrPrice, price),
                signed_field(FieldCode::kFortsAggrReplOrdersAggrVolume, volume),
            };
            require(!slots.on_row(row), "slot update must stage");
        };
        slots.begin_transaction();
        stage(1, 4433036, 1, "12.936", 10);
        stage(2, 4433036, 2, "12.937", 10);
        require(!slots.commit(), "initial slot commit");
        slots.begin_transaction();
        stage(1, 4433036, 1, "12.927", 10);
        stage(2, 4433036, 2, "12.928", 10);
        require(slots.snapshot_for_isin(4433036)->top_bid->price == "12.936",
                "uncommitted moves must remain invisible");
        require(!slots.commit(), "price moves commit");
        const auto moved = slots.snapshot_for_isin(4433036);
        require(moved->row_count == 2 && moved->top_bid->price == "12.927" && moved->top_ask->price == "12.928",
                "row replacement must not manufacture the old-bid/new-ask cross");
        slots.begin_transaction();
        stage(1, 4433036, 2, "12.929", 9);
        require(!slots.commit(), "side move commit");
        require(!slots.snapshot_for_isin(4433036)->top_bid && slots.snapshot_for_isin(4433036)->row_count == 2,
                "side move must remove previous bid");
        slots.begin_transaction();
        stage(1, 4433036, 1, "0", 0);
        require(!slots.commit(), "zero-volume identity deletion commit");
        require(slots.snapshot_for_isin(4433036)->row_count == 1,
                "deletion must find the slot even when price and direction change");
        slots.begin_transaction();
        stage(2, 4433036, 1, "0", 10, 1);
        require(!slots.commit(), "replAct deletion commit");
        require(slots.snapshot_for_isin(4433036)->row_count == 0,
                "replication tombstone must remove the slot despite positive volume");
        slots.begin_transaction();
        stage(3, 4433036, 1, "12.927", 10);
        stage(3, 4433036, 1, "12.926", 11);
        require(!slots.commit(), "multiple updates to same slot commit");
        require(slots.snapshot_for_isin(4433036)->row_count == 1 &&
                    slots.snapshot_for_isin(4433036)->top_bid->price == "12.926",
                "last in-transaction slot update wins");
        slots.begin_transaction();
        stage(3, 1001, 2, "100", 10);
        slots.rollback();
        require(slots.snapshot_for_isin(4433036)->row_count == 1 && !slots.snapshot_for_isin(1001),
                "rollback must preserve original instrument ownership");
        slots.begin_transaction();
        stage(3, 1001, 2, "100", 10);
        require(!slots.commit(), "instrument move commit");
        require(slots.snapshot_for_isin(4433036)->row_count == 0 && slots.snapshot_for_isin(1001)->row_count == 1,
                "instrument move must rebuild both old and new instrument snapshots");

        Plaza2Aggr20BookProjector signed_book;
        const auto stage_signed = [&](std::uint64_t id, std::int64_t dir, std::string_view price) {
            const std::array row = {
                unsigned_field(FieldCode::kFortsAggrReplOrdersAggrReplId, id),
                signed_field(FieldCode::kFortsAggrReplOrdersAggrReplRev, static_cast<std::int64_t>(id)),
                signed_field(FieldCode::kFortsAggrReplOrdersAggrIsinId, 3003),
                signed_field(FieldCode::kFortsAggrReplOrdersAggrDir, dir),
                decimal_field(FieldCode::kFortsAggrReplOrdersAggrPrice, price),
                signed_field(FieldCode::kFortsAggrReplOrdersAggrVolume, 1),
            };
            require(!signed_book.on_row(row), "signed AGGR20 row must stage");
        };
        signed_book.begin_transaction();
        stage_signed(11, 1, "-100.50000");
        stage_signed(12, 1, "0");
        stage_signed(13, 1, "100.50000");
        stage_signed(14, 2, "-100.50000");
        stage_signed(15, 2, "0.00000");
        stage_signed(16, 2, "100.50000");
        require(!signed_book.commit(), "signed AGGR20 rows must commit");
        const auto signed_snapshot = signed_book.snapshot_for_isin(3003);
        require(signed_snapshot.has_value() && signed_snapshot->top_bid.has_value() &&
                    signed_snapshot->top_ask.has_value() && signed_snapshot->top_bid->price == "100.50000" &&
                    signed_snapshot->top_bid->price_scaled == 10'050'000 &&
                    signed_snapshot->top_ask->price == "-100.50000" &&
                    signed_snapshot->top_ask->price_scaled == -10'050'000,
                "signed AGGR20 BBO must preserve negative, zero, and positive values");
        require(signed_snapshot->levels.size() == 6 && signed_snapshot->levels[0].price == "100.50000" &&
                    signed_snapshot->levels[1].price == "0" && signed_snapshot->levels[2].price == "-100.50000" &&
                    signed_snapshot->levels[3].price == "-100.50000" && signed_snapshot->levels[4].price == "0.00000" &&
                    signed_snapshot->levels[5].price == "100.50000",
                "signed AGGR20 levels must sort bids descending and asks ascending");
        slots.reset();
        require(slots.snapshot().row_count == 0 && !slots.snapshot_for_isin(1001),
                "epoch reset must discard slot state");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
