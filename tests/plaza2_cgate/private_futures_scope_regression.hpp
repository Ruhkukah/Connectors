#pragma once

#include "private_state_deletion_regression.hpp"
#include <chrono>
#include <limits>

namespace moex::plaza2::test {
inline constexpr std::array unconsumed_private_tables{
    generated::TableCode::kFortsTradeReplMultilegOrdersLog,
    generated::TableCode::kFortsTradeReplUserMultilegDeal,
    generated::TableCode::kFortsUserorderbookReplMultilegOrders,
    generated::TableCode::kFortsUserorderbookReplOrdersCurrentday,
    generated::TableCode::kFortsUserorderbookReplMultilegOrdersCurrentday,
    generated::TableCode::kFortsUserorderbookReplInfoCurrentday,
    generated::TableCode::kFortsPosReplPositionSa,
    generated::TableCode::kFortsPartReplPart,
    generated::TableCode::kFortsPartReplPartSa,
    generated::TableCode::kFortsRefdataReplOptSessContents,
    generated::TableCode::kFortsRefdataReplMultilegDict,
    generated::TableCode::kFortsRefdataReplInstr2matchingMap};

inline void private_futures_scope_regression() {
    using namespace deletion;
    Harness h;
    h.begin(kFortsTradeReplOrdersLog, true);
    h.row(kFortsTradeReplOrdersLog, 10, 1);
    h.row(kFortsTradeReplUserDeal, 20, 2);
    h.commit();
    h.begin(kFortsRefdataReplFutInstruments, true);
    h.row(kFortsRefdataReplFutInstruments, 30, 1);
    h.commit();
    h.begin(kFortsPosReplPosition, true);
    h.row(kFortsPosReplPosition, 40, 1);
    h.commit();
    (void)h.projector.take_row_changes();
    for (const auto table : unconsumed_private_tables) {
        h.begin(table, true);
        h.row(table, 100, 1);
        h.commit();
        require(h.projector.own_orders().size() == 1 && h.projector.own_trades().size() == 1 &&
                    h.projector.instruments().size() == 1 && h.projector.positions().size() == 1 &&
                    h.projector.limits().empty() && h.projector.matching_map().empty(),
                "undeclared private table created a product projection");
        require(h.health(table).committed_row_count > 0,
                "ignoring unused rows discarded required stream commit/row health");
        h.begin(table, true);
        h.row(table, 100, 2, 1, true);
        h.projector.on_event({},
                             {.kind = pr::EventKind::kClearDeleted,
                              .stream_code = stream(table),
                              .table_code = table,
                              .signed_value = std::numeric_limits<std::int64_t>::max()},
                             h.state);
        h.commit();
        require(h.projector.own_orders().size() == 1 && h.projector.own_trades().size() == 1 &&
                    h.projector.instruments().size() == 1 && h.projector.positions().size() == 1,
                "undeclared row deletion/clear changed a retained product projection");
    }
    const auto loss = h.projector.take_row_changes();
    require(loss.trade_history_truncated && !loss.regular_trade_history_truncated,
            "ignored multileg clears lost broad history diagnostics or invalidated regular execution");
}

inline void private_userbook_refresh_regression() {
    using namespace deletion;
    Harness h;
    constexpr std::int64_t count = 150000;
    h.begin(kFortsTradeReplOrdersLog, true);
    for (std::int64_t id = 1; id <= count; ++id)
        h.row(kFortsTradeReplOrdersLog, id, id);
    h.commit();
    (void)h.projector.take_row_changes();
    h.begin(kFortsUserorderbookReplOrders, true);
    h.row(kFortsUserorderbookReplOrders, count + 1, 1);
    h.row(kFortsUserorderbookReplOrders, count + 2, 2);
    h.row(kFortsUserorderbookReplInfo, count, 3);
    h.commit();
    (void)h.projector.take_row_changes();
    const auto* original_view = h.projector.own_orders().data();
    const auto start = std::chrono::steady_clock::now();
    h.begin(kFortsUserorderbookReplOrders, true);
    h.projector.on_event({},
                         {.kind = pr::EventKind::kClearDeleted,
                          .stream_code = gen::StreamCode::kFortsUserorderbookRepl,
                          .table_code = kFortsUserorderbookReplOrders,
                          .signed_value = 2},
                         h.state);
    h.row(kFortsUserorderbookReplOrders, count + 1, 4);
    h.row(kFortsUserorderbookReplInfo, count, 5);
    h.commit();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    const auto changes = h.projector.take_row_changes();
    require(h.projector.own_orders().size() == count + 2 && h.has(kFortsTradeReplOrdersLog, 1) &&
                h.has(kFortsTradeReplOrdersLog, count) && h.has(kFortsUserorderbookReplOrders, count + 2) &&
                h.health(kFortsUserorderbookReplOrders).periodic_snapshot_consistent,
            "USERORDERBOOK clear/reinsert lost unrelated TRADE, surviving book rows or committed marker");
    require(changes.orders.empty() && changes.trades.empty() && !changes.resync_required,
            "periodic book refresh became a trading resync/delta");
    std::cout << "UOB clear/reinsert alongside 150000 TRADE rows: " << elapsed << " us\n";
    require(h.projector.own_orders().data() == original_view,
            "periodic USERORDERBOOK refresh rebuilt the complete TRADE view");
#if MOEX_RELEASE_PERFORMANCE_ACCEPTANCE
    require(elapsed < 1000, "periodic single-row USERORDERBOOK refresh exceeds Release 1ms acceptance");
#endif
    // Sparse deletion and same-transaction reinsertion use the same canonical
    // view index; both deleted back slots must remain resolvable until commit.
    h.begin(kFortsUserorderbookReplOrders, true);
    h.row(kFortsUserorderbookReplOrders, count + 1, 6, 1, true);
    h.row(kFortsUserorderbookReplOrders, count + 2, 7, 1, true);
    h.commit();
    require(h.projector.own_orders().size() == count && h.has(kFortsTradeReplOrdersLog, count),
            "multiple book deletions compacted an unrelated TRADE slot");
    h.begin(kFortsUserorderbookReplOrders, true);
    h.row(kFortsUserorderbookReplOrders, count + 3, 8);
    h.commit();
    h.begin(kFortsUserorderbookReplOrders, true);
    h.projector.on_event({},
                         {.kind = pr::EventKind::kClearDeleted,
                          .stream_code = gen::StreamCode::kFortsUserorderbookRepl,
                          .table_code = kFortsUserorderbookReplOrders,
                          .signed_value = std::numeric_limits<std::int64_t>::max()},
                         h.state);
    h.commit();
    require(h.projector.own_orders().size() == count, "clear-all last book source corrupted retained TRADE view/index");
}
} // namespace moex::plaza2::test
