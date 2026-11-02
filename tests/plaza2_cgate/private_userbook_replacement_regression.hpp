#pragma once

#include "private_state_deletion_regression.hpp"

namespace moex::plaza2::test {
inline void private_userbook_replacement_regression() {
    using namespace deletion;
    for (const auto replacement : {0, 1, 2}) {
        Harness h;
        h.begin(kFortsTradeReplOrdersLog, true);
        h.row(kFortsTradeReplOrdersLog, 10, 1);
        h.row(kFortsTradeReplUserDeal, 20, 2);
        h.commit();
        (void)h.projector.take_row_changes();
        h.begin(kFortsUserorderbookReplOrders, true);
        h.row(kFortsUserorderbookReplOrders, 10, 1);
        h.row(kFortsUserorderbookReplOrders, 30, 2);
        h.row(kFortsUserorderbookReplInfo, 100, 3);
        h.commit();
        (void)h.projector.take_row_changes();
        h.begin(kFortsTradeReplOrdersLog, true);
        h.row(kFortsTradeReplOrdersLog, 40, 4);
        h.row(kFortsTradeReplUserDeal, 50, 5);
        h.commit(); // Leave both TRADE deltas queued across the book replacement.
        const auto* view = h.projector.own_orders().data();
        h.begin(kFortsUserorderbookReplOrders, true);
        if (replacement == 2)
            h.row(kFortsUserorderbookReplOrders, 60, 6, 0, false, 10030);
        else
            h.row(kFortsUserorderbookReplOrders, 30, 6, 1, replacement == 1);
        h.row(kFortsUserorderbookReplInfo, 100, 7);
        h.commit();
        const auto changes = h.projector.take_row_changes();
        require(!changes.resync_required && !changes.trade_history_truncated &&
                    !changes.regular_trade_history_truncated,
                "USERORDERBOOK tombstone/replID reuse triggered TRADE history reconstruction");
        require(changes.orders.size() == 1 && changes.orders[0].public_order_id == 40 && changes.trades.size() == 1 &&
                    changes.trades[0].id_deal == 50,
                "USERORDERBOOK replacement discarded or added to pending TRADE deltas");
        require(h.projector.own_orders().data() == view && h.has(kFortsTradeReplOrdersLog, 10) &&
                    h.has(kFortsTradeReplOrdersLog, 40) && !h.has(kFortsUserorderbookReplOrders, 30) &&
                    h.has(kFortsUserorderbookReplOrders, 60) == (replacement == 2) &&
                    h.projector.own_trades().size() == 2 &&
                    h.health(kFortsUserorderbookReplOrders).periodic_snapshot_consistent,
                "USERORDERBOOK replacement damaged an unrelated source, view index or committed marker");
        // Actual TRADE deletion must still invalidate its incremental history.
        h.begin(kFortsTradeReplOrdersLog, true);
        h.row(kFortsTradeReplOrdersLog, 40, 8, 1, true);
        h.commit();
        const auto loss = h.projector.take_row_changes();
        require(loss.resync_required && loss.trade_history_truncated && loss.regular_trade_history_truncated,
                "regular TRADE tombstone lost its required reconstruction signal");
    }
}
} // namespace moex::plaza2::test
