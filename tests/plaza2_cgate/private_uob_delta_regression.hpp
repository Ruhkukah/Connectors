#pragma once

#include "private_state_deletion_regression.hpp"

#include <limits>

namespace moex::plaza2::test {
inline void private_uob_delta_regression() {
    using namespace deletion;
    constexpr std::int64_t count = ps::kPrivateRowChangeCapacity + 1;
    for (const auto table : {kFortsUserorderbookReplOrders}) {
        Harness h;
        const auto publish = [&](std::int64_t revision, bool republish) {
            h.begin(table, true);
            if (republish)
                h.projector.on_event({},
                                     {.kind = pr::EventKind::kClearDeleted,
                                      .stream_code = gen::StreamCode::kFortsUserorderbookRepl,
                                      .table_code = table,
                                      .signed_value = std::numeric_limits<std::int64_t>::max()},
                                     h.state);
            for (std::int64_t id = 1; id <= count; ++id)
                h.row(table, id, revision + id);
            h.row(kFortsUserorderbookReplInfo, count + 1, revision + count + 1);
            h.commit();
        };
        publish(0, false);
        const auto initial = h.projector.take_row_changes();
        require(!initial.resync_required && initial.orders.empty() && initial.trades.empty() &&
                    !initial.trade_history_truncated,
                "large USERORDERBOOK snapshot overflowed the trading delta buffer");
        require(h.projector.own_orders().size() == count && h.health(table).periodic_snapshot_consistent,
                "USERORDERBOOK snapshot or committed publication marker was lost");

        h.begin(kFortsTradeReplOrdersLog, true);
        h.row(kFortsTradeReplOrdersLog, 50000, 1);
        h.row(kFortsTradeReplUserDeal, 60000, 2);
        h.commit(); // Leave real trading deltas undrained during the periodic republish.
        publish(2 * count, true);
        const auto changes = h.projector.take_row_changes();
        require(!changes.resync_required && !changes.trade_history_truncated && changes.orders.size() == 1 &&
                    changes.orders[0].from_trade_repl && changes.orders[0].public_order_id == 50000 &&
                    changes.trades.size() == 1 && changes.trades[0].id_deal == 60000,
                "USERORDERBOOK republish discarded or crowded out undrained TRADE changes");
        require(h.projector.own_orders().size() == count + 1 && h.projector.own_trades().size() == 1 &&
                    h.health(table).periodic_snapshot_consistent && h.health(table).last_commit_sequence == 3 &&
                    h.health(table).committed_row_count == 2 * (count + 1),
                "periodic republish lost committed rows, source views, or commit/row watermarks");

        h.begin(kFortsTradeReplOrdersLog, true);
        for (std::int64_t id = 100000; id < 100000 + ps::kPrivateRowChangeCapacity; ++id)
            h.row(kFortsTradeReplOrdersLog, id, id);
        h.commit();
        publish(4 * count, true);
        const auto boundary = h.projector.take_row_changes();
        require(!boundary.resync_required && boundary.orders.size() == ps::kPrivateRowChangeCapacity &&
                    std::all_of(boundary.orders.begin(), boundary.orders.end(),
                                [](const auto& order) { return order.from_trade_repl; }),
                "USERORDERBOOK republish consumed the exact 8192-row TRADE capacity");

        h.begin(kFortsTradeReplOrdersLog, true);
        for (std::int64_t id = 100000; id <= 100000 + ps::kPrivateRowChangeCapacity; ++id)
            h.row(kFortsTradeReplOrdersLog, id, id + 1);
        h.commit();
        const auto overflow = h.projector.take_row_changes();
        require(overflow.resync_required && overflow.regular_trade_history_truncated && overflow.orders.empty() &&
                    h.projector.own_orders().size() == count + 2 + ps::kPrivateRowChangeCapacity,
                "actual TRADE overflow stopped preserving its full committed snapshot and resync guard");
    }
}
} // namespace moex::plaza2::test
