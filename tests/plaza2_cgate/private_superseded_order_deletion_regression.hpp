#pragma once

#include "private_state_deletion_regression.hpp"

namespace moex::plaza2::test {
inline void private_superseded_order_deletion_regression() {
    using namespace deletion;
    using enum gen::FieldCode;
    for (const auto table : {kFortsTradeReplOrdersLog, kFortsUserorderbookReplOrders}) {
        for (const auto retirement : {0, 1, 2, 3}) {
            Harness h;
            const auto row = [&](std::int64_t id, std::int64_t revision, bool linked = false) {
                Row values(table, id, revision);
                for (auto& field : values.fields) {
                    if (field.field_code == kFortsTradeReplOrdersLogPrivateOrderId ||
                        field.field_code == kFortsUserorderbookReplOrdersPrivateOrderId)
                        field.signed_value = linked ? 10 : id;
                    if (field.field_code == kFortsTradeReplOrdersLogClientCode ||
                        field.field_code == kFortsUserorderbookReplOrdersClientCode)
                        field.text_value = "OWN";
                    if (field.field_code == kFortsTradeReplOrdersLogExtId ||
                        field.field_code == kFortsUserorderbookReplOrdersExtId)
                        field.signed_value = 17;
                    if (field.field_code == kFortsTradeReplOrdersLogSessId ||
                        field.field_code == kFortsUserorderbookReplOrdersSessId)
                        field.signed_value = linked ? 322 : 321;
                    if (field.field_code == kFortsTradeReplOrdersLogIdOrd1 ||
                        field.field_code == kFortsUserorderbookReplOrdersIdOrd1)
                        field.signed_value = linked ? 10 : 0;
                }
                h.projector.on_stream_row({},
                                          {.kind = pr::EventKind::kStreamData,
                                           .stream_code = stream(table),
                                           .table_code = table,
                                           .signed_value = revision},
                                          {}, values.fields, h.state);
                ++h.rows;
            };
            h.begin(table, true);
            row(10, 1);
            row(30, 3); // An unrelated working order must survive every operation.
            h.commit();
            h.begin(table, true);
            row(20, 4, true);
            h.commit();
            (void)h.projector.take_row_changes();
            require(h.projector.own_orders().size() == 2,
                    "superseded-row fixture did not preserve its merged identity and unrelated order");
            h.begin(table); // A synthetic rollback must retain physical ownership.
            h.row(table, 20, 5, 1, true);
            h.begin(table);
            h.commit();
            require(h.projector.own_orders().size() == 2, "rolled-back current-row deletion escaped its transaction");
            h.begin(table, true);
            if (retirement < 2)
                h.row(table, 10, 5, 1, retirement == 1);
            else if (retirement == 2)
                h.projector.on_event({},
                                     {.kind = pr::EventKind::kClearDeleted,
                                      .stream_code = stream(table),
                                      .table_code = table,
                                      .signed_value = 2},
                                     h.state);
            else
                h.row(table, 40, 5, 0, false, 10010);
            h.commit();
            const auto orders = h.projector.own_orders();
            const auto current = std::find_if(orders.begin(), orders.end(),
                                              [](const auto& value) { return value.private_order_id == 10; });
            require(current != orders.end() && current->repl_id == 10020 && current->sess_id == 322 &&
                        current->id_ord1 == 10 && current->public_amount_rest == 20 &&
                        std::find(current->public_order_id_aliases.begin(), current->public_order_id_aliases.end(),
                                  20) != current->public_order_id_aliases.end() &&
                        orders.size() == (retirement == 3 ? 3u : 2u) && h.has(table, 30),
                    "retiring the superseded physical row erased the live relisted order");
            const auto loss = h.projector.take_row_changes();
            require(table != kFortsTradeReplOrdersLog || loss.regular_trade_history_truncated,
                    "superseded TRADE retirement lost its conservative history-loss signal");
            require(table != kFortsUserorderbookReplOrders || !loss.resync_required,
                    "superseded USERORDERBOOK retirement invalidated TRADE history");
            // The newer physical row remains independently deletable after the
            // obsolete one is retired, including through a cloned source index.
            h.projector = h.projector.clone();
            h.begin(table, true);
            h.row(table, 20, 6, 1, true);
            h.commit();
            require(h.projector.own_orders().size() == (retirement == 3 ? 2u : 1u) && h.has(table, 30) &&
                        std::none_of(h.projector.own_orders().begin(), h.projector.own_orders().end(),
                                     [](const auto& value) { return value.private_order_id == 10; }),
                    "deletion of the current physical row left its merged order or damaged another order");
        }
    }
}
} // namespace moex::plaza2::test
