#include "moex/connector_host/full_order_log_dtc.hpp"
#include "plaza2_cgate/full_order_log_test_support.hpp"
#include <iostream>
using namespace moex::connector_host::dtc;
using full_order_log_test::require;
int main() {
    try {
        full_order_log_test::Harness h;
        h.online();
        h.begin();
        h.add(1, 11, 10000000, 1);
        h.add(2, 11, 9000000, 2);
        h.add(3, 11, 8000000, 3);
        h.add(10, 11, 11000000, 4, 1, 1, 1, 2);
        h.add(11, 11, 12000000, 5, 1, 1, 1, 2);
        h.commit();
        DtcMarketDataSnapshot metadata;
        metadata.transport_active = metadata.refdata_metadata_current = true;
        DtcFullOrderLogSource source(h.book, 11, metadata, 32);
        source.configure_depth_limit(2);
        h.book.on_commit = [&](const auto&) { source.committed(); };
        auto snapshot = source.snapshot();
        require(snapshot.levels.size() == 4 && snapshot.levels[0].price_scaled == 10000000 &&
                    snapshot.levels[1].price_scaled == 9000000,
                "client top-N snapshot honors configured depth");
        require(snapshot.full_order_log && !snapshot.order_entry_allowed && snapshot.source_snapshot_hash == 0 &&
                    snapshot.levels[0].source_row_id == 0,
                "anonymous aggregation creates no provenance identities");
        h.begin();
        h.add(4, 11, 10500000, 8);
        require(source.snapshot().levels[0].price_scaled == 10000000, "pending rows are invisible to DTC");
        h.commit();
        auto updates = source.depth_changes();
        require(updates.size() == 2 && updates[0].depth_level == 1 && updates[0].price_scaled == 10500000 &&
                    updates[1].depth_level == 2 && updates[1].price_scaled == 10000000,
                "better price shifts every changed visible position atomically");
        h.begin();
        h.add(5, 11, 7000000, 9);
        h.commit();
        require(source.depth_changes().empty(), "deeper book activity emits no out-of-range positions");
        h.begin();
        h.add(4, 11, 10500000, 8, 0);
        h.commit();
        require(source.depth_changes().size() == 2 && source.depth_changes()[0].price_scaled == 10000000 &&
                    source.depth_changes()[1].price_scaled == 9000000,
                "removed best price refills both positions");
        h.begin();
        h.add(1, 11, 10000000, 1, 0);
        h.commit();
        require(source.depth_changes().size() == 2 && source.depth_changes()[1].price_scaled == 8000000,
                "previously hidden third level enters the subscribed range");
        h.begin();
        h.add(3, 11, 8000000, 3, 0);
        h.add(5, 11, 7000000, 9, 0);
        h.commit();
        require(source.depth_changes().size() == 1 && source.depth_changes()[0].depth_level == 2 &&
                    source.depth_changes()[0].volume == 0,
                "trailing position deleted when subscribed side shrinks");
        h.begin();
        h.add(2, 11, 9000000, 2, 0);
        h.add(10, 11, 11000000, 4, 0, 1, 1, 2);
        h.add(11, 11, 12000000, 5, 0, 1, 1, 2);
        h.commit();
        require(source.depth_changes().size() == 3 && source.snapshot().levels.empty(),
                "all visible positions can be cleared");
        for (const auto& row : source.depth_changes())
            require(row.volume == 0 && row.depth_level > 0, "complete depletion uses deletions");
        h.begin();
        h.add(20, 22, 10000000, 5);
        h.commit();
        require(source.depth_changes().empty(), "configured instruments remain isolated in one owner");
        h.begin();
        h.add(21, 11, 1677721600000LL, 1);
        h.add(22, 11, 1677721700000LL, 1);
        h.commit();
        require(!source.status_snapshot().market_data_display_allowed, "float32 price collision fences publication");
        h.control(Plaza2ListenerEventKind::Close);
        require(!source.status_snapshot().book_snapshot_current, "recovery invalidates DTC authority immediately");
        full_order_log_test::Harness recovery;
        recovery.online();
        recovery.begin();
        recovery.add(1, 11, 1677721600000LL, 1);
        recovery.commit();
        DtcFullOrderLogSource recover_source(recovery.book, 11, metadata, 32);
        recover_source.configure_depth_limit(2);
        recovery.book.on_commit = [&](const auto&) { recover_source.committed(); };
        recovery.begin();
        recovery.add(2, 11, 1677721700000LL, 1);
        recovery.commit();
        require(!recover_source.status_snapshot().valid && recovery.book.revision(11) == 2,
                "collision at old revision");
        recovery.control(Plaza2ListenerEventKind::Close);
        recovery.open();
        recovery.begin();
        recovery.add(3, 11, 10000000, 1);
        recovery.commit();
        recovery.begin();
        recovery.add(4, 11, 9000000, 1);
        recovery.commit();
        recovery.online();
        recovery.begin();
        recovery.commit();
        require(recovery.book.revision(11) == 2 && recover_source.status_snapshot().valid,
                "new stream epoch clears cached collision even at the same book revision");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "FullOrderLog DTC source: committed top-N, shifted positions, deletion, isolation and float collision "
                 "passed\n";
}
