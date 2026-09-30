#include "moex/plaza2/cgate/plaza2_aggr20_md.hpp"
#include "plaza2_runtime_test_support.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
using namespace moex::plaza2::cgate;
using moex::plaza2::test::require;
using F = moex::plaza2::generated::FieldCode;
using T = moex::plaza2::generated::TableCode;
Plaza2DecodedFieldValue number(F field, std::int64_t value) {
    return {.field_code = field, .kind = Plaza2DecodedValueKind::SignedInteger, .signed_value = value};
}
auto row(std::uint64_t id, std::int64_t revision, std::int64_t isin = 1001, std::int64_t volume = 1) {
    return std::array{number(F::kFortsAggrReplOrdersAggrReplId, id),
                      number(F::kFortsAggrReplOrdersAggrReplRev, revision),
                      number(F::kFortsAggrReplOrdersAggrIsinId, isin),
                      number(F::kFortsAggrReplOrdersAggrVolume, volume),
                      number(F::kFortsAggrReplOrdersAggrDir, id % 2 + 1),
                      Plaza2DecodedFieldValue{.field_code = F::kFortsAggrReplOrdersAggrPrice,
                                              .kind = Plaza2DecodedValueKind::Decimal,
                                              .text_value = "100.00000",
                                              .decimal_mantissa = 10'000'000 + static_cast<std::int64_t>(id),
                                              .decimal_scale = 5,
                                              .decimal_exact = true}};
}
int main() {
    try {
        Plaza2Aggr20BookProjector projector;
        const auto started = std::chrono::steady_clock::now();
        projector.begin_transaction();
        for (std::uint64_t i = 1; i <= 160'000; ++i)
            require(!projector.on_row(row(i, i)), "snapshot row decoded");
        require(projector.snapshot().row_count == 0, "snapshot rows stay staged until commit");
        require(!projector.commit(), "large snapshot commits");
        const auto elapsed = std::chrono::steady_clock::now() - started;
        std::cout << "160k snapshot_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << '\n';
        // This catches the previously measured 33.8s initial-book quadratic path.
        require(elapsed < std::chrono::seconds(1), "160k initial snapshot must complete under one second");
        auto initial = projector.snapshot_for_isin(1001);
        require(initial && initial->row_count == 160'000 && initial->bid_depth_levels == 80'000 &&
                    initial->ask_depth_levels == 80'000,
                "large snapshot has correct sides and row count");
        projector.begin_transaction();
        require(!projector.on_row(row(160'001, 160'001, 2002)), "unrelated instrument row decoded");
        require(!projector.commit(), "unrelated commit succeeds");
        require(projector.snapshot_for_isin(1001)->source_snapshot_version == initial->source_snapshot_version,
                "unrelated instrument changes do not advance target version");
        projector.begin_transaction();
        require(!projector.on_row(row(1, 200'000, 1001, 4)), "update staged");
        projector.rollback();
        require(projector.snapshot_for_isin(1001)->source_snapshot_version == initial->source_snapshot_version,
                "rollback does not expose staged updates");
        Plaza2Aggr20ListenerBridge bridge(projector, 321);
        // Test cleanup without OPEN (which deliberately resets the projection).
        const auto clear = [&](T table, std::int64_t revision) {
            require(!bridge.on_plaza2_listener_event(
                        {.kind = Plaza2ListenerEventKind::ClearDeleted, .table_code = table, .signed_value = revision}),
                    "ClearDeleted succeeds");
        };
        clear(T::kFortsAggrReplSysEvents, std::numeric_limits<std::int64_t>::max());
        require(projector.snapshot().row_count == 160'001 && !bridge.recovering(),
                "unrelated table does not clear book or reopen");
        clear(T::kFortsAggrReplOrdersAggr, 80'001);
        require(projector.snapshot_for_isin(1001)->row_count == 80'000 && !bridge.recovering(),
                "normal mid-session cleanup removes exactly revisions below threshold without teardown");
        clear(T::kFortsAggrReplOrdersAggr, std::numeric_limits<std::int64_t>::max());
        require(projector.snapshot().row_count == 0 && !bridge.recovering(),
                "MAX cleanup empties book without teardown");
        projector.begin_transaction();
        require(!projector.on_row(row(1, 1)), "revisions may restart after MAX cleanup");
        require(!projector.commit() && projector.snapshot_for_isin(1001)->row_count == 1, "post-MAX book resumes");
        projector.begin_transaction();
        const std::array deletion{number(F::kFortsAggrReplOrdersAggrReplId, 1),
                                  number(F::kFortsAggrReplOrdersAggrReplRev, 2),
                                  number(F::kFortsAggrReplOrdersAggrReplAct, 1)};
        require(!projector.on_row(deletion), "NULL price deletion uses replID only");
        require(projector.snapshot().row_count == 1, "delete remains staged");
        require(!projector.commit() && projector.snapshot().row_count == 0, "NULL price deletion commits correctly");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
