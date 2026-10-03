#include "moex/plaza2/cgate/plaza2_private_state_bridge.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace moex::plaza2;
using namespace moex::plaza2::cgate;
using enum generated::StreamCode;
using enum generated::FieldCode;
using enum generated::TableCode;
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void bridge_owns_callback_values() {
    private_state::Plaza2PrivateStateProjector projector;
    Plaza2PrivateStateBridge bridge(projector);
    const std::array streams{kFortsTradeRepl};
    require(!bridge.reset(streams) && !bridge.begin_run(), "borrowed values begin run");
    const auto event = [&](Plaza2ListenerEventKind kind) {
        require(!bridge.on_plaza2_listener_event({.kind = kind, .stream_code = kFortsTradeRepl}),
                "borrowed values transaction event");
    };
    std::string client = "callback account beyond small string storage";
    std::string comment = "callback comment: \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82";
    std::string price = "123.45";
    const auto expected_client = client;
    const auto expected_comment = comment;
    const auto expected_price = price;
    std::array fields{Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicOrderId,
                                              .kind = Plaza2DecodedValueKind::SignedInteger,
                                              .signed_value = 92},
                      Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPrivateOrderId,
                                              .kind = Plaza2DecodedValueKind::SignedInteger,
                                              .signed_value = 92},
                      Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogClientCode,
                                              .kind = Plaza2DecodedValueKind::String,
                                              .text_value = client},
                      Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogComment,
                                              .kind = Plaza2DecodedValueKind::String,
                                              .text_value = comment},
                      Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPrice,
                                              .kind = Plaza2DecodedValueKind::Decimal,
                                              .text_value = price},
                      Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogMoment,
                                              .kind = Plaza2DecodedValueKind::Timestamp,
                                              .unsigned_value = 1791000000,
                                              .timestamp_ns = 1791000000123456789},
                      Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicAmountRest,
                                              .kind = Plaza2DecodedValueKind::UnsignedInteger,
                                              .unsigned_value = 7},
                      Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicAmount,
                                              .kind = Plaza2DecodedValueKind::None,
                                              .signed_value = 99}};
    const auto row = [&](std::int64_t revision) {
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .fields = fields,
                                                  .signed_value = revision}),
                "borrowed values row");
    };
    event(Plaza2ListenerEventKind::TransactionBegin);
    row(1);
    // Callback storage is reusable before commit. A second operation also
    // moves the pending vector, so its text cannot borrow the first owner's SSO.
    fields[0].signed_value = fields[1].signed_value = 91;
    fields[4].kind = Plaza2DecodedValueKind::FloatingPoint;
    row(2);
    client.assign(512, 'a');
    comment.assign(512, 'b');
    price.assign(512, 'c');
    require(projector.own_orders().empty(), "borrowed values exposed before commit");
    event(Plaza2ListenerEventKind::TransactionCommit);
    const auto orders = projector.own_orders();
    require(orders.size() == 2 && orders[0].public_order_id == 91 && orders[1].public_order_id == 92,
            "native snapshot order changed while sorting references");
    for (const auto& order : orders)
        require(order.client_code == expected_client && order.comment == expected_comment &&
                    order.price == expected_price && order.moment == 1791000000 &&
                    order.moment_ns == 1791000000123456789 && order.public_amount_rest == 7 && order.public_amount == 0,
                "commit borrowed mutated callback text or lost decoded values");
}

void terminal_trade_preserves_independent_userbook() {
    for (const bool book_first : {false, true}) {
        for (const bool interleaved : {false, true}) {
            private_state::Plaza2PrivateStateProjector projector;
            Plaza2PrivateStateBridge bridge(projector);
            const std::array streams{kFortsTradeRepl, kFortsUserorderbookRepl};
            require(!bridge.reset(streams) && !bridge.begin_run(), "independent terminal surfaces begin run");
            const auto integer = [](generated::FieldCode code, std::int64_t value) {
                return Plaza2DecodedFieldValue{
                    .field_code = code, .kind = Plaza2DecodedValueKind::SignedInteger, .signed_value = value};
            };
            const std::array trade_fields{integer(kFortsTradeReplOrdersLogPublicOrderId, 91001),
                                          integer(kFortsTradeReplOrdersLogPrivateOrderId, 91001),
                                          integer(kFortsTradeReplOrdersLogSessId, 321),
                                          integer(kFortsTradeReplOrdersLogIsinId, 1001),
                                          integer(kFortsTradeReplOrdersLogDir, 1),
                                          integer(kFortsTradeReplOrdersLogPublicAmount, 2),
                                          integer(kFortsTradeReplOrdersLogPrivateAmount, 2),
                                          integer(kFortsTradeReplOrdersLogPublicAmountRest, 0),
                                          integer(kFortsTradeReplOrdersLogPrivateAmountRest, 0),
                                          integer(kFortsTradeReplOrdersLogPublicAction, 2),
                                          integer(kFortsTradeReplOrdersLogPrivateAction, 2)};
            const std::array book_fields{integer(kFortsUserorderbookReplOrdersPublicOrderId, 91001),
                                         integer(kFortsUserorderbookReplOrdersPrivateOrderId, 91001),
                                         integer(kFortsUserorderbookReplOrdersSessId, 321),
                                         integer(kFortsUserorderbookReplOrdersIsinId, 1001),
                                         integer(kFortsUserorderbookReplOrdersDir, 1),
                                         integer(kFortsUserorderbookReplOrdersPublicAmount, 2),
                                         integer(kFortsUserorderbookReplOrdersPrivateAmount, 2),
                                         integer(kFortsUserorderbookReplOrdersPublicAmountRest, 2),
                                         integer(kFortsUserorderbookReplOrdersPrivateAmountRest, 2),
                                         integer(kFortsUserorderbookReplOrdersPublicAction, 1),
                                         integer(kFortsUserorderbookReplOrdersPrivateAction, 1)};
            const auto event = [&](Plaza2ListenerEventKind kind, generated::StreamCode stream) {
                require(!bridge.on_plaza2_listener_event({.kind = kind, .stream_code = stream}),
                        "independent terminal surface transaction event");
            };
            const auto row = [&](generated::StreamCode stream) {
                require(!bridge.on_plaza2_listener_event(
                            {.kind = Plaza2ListenerEventKind::StreamData,
                             .stream_code = stream,
                             .table_code =
                                 stream == kFortsTradeRepl ? kFortsTradeReplOrdersLog : kFortsUserorderbookReplOrders,
                             .fields = stream == kFortsTradeRepl ? std::span(trade_fields) : std::span(book_fields),
                             .signed_value = 1}),
                        "independent terminal surface row");
            };
            const auto first = book_first ? kFortsUserorderbookRepl : kFortsTradeRepl;
            const auto second = book_first ? kFortsTradeRepl : kFortsUserorderbookRepl;
            event(Plaza2ListenerEventKind::TransactionBegin, first);
            row(first);
            if (!interleaved)
                event(Plaza2ListenerEventKind::TransactionCommit, first);
            event(Plaza2ListenerEventKind::TransactionBegin, second);
            row(second);
            event(Plaza2ListenerEventKind::TransactionCommit, second);
            if (interleaved)
                event(Plaza2ListenerEventKind::TransactionCommit, first);
            const auto orders = projector.own_orders();
            require(orders.size() == 2, "independent terminal surfaces were coalesced");
            for (const auto& order : orders) {
                require(order.public_order_id == 91001 && order.private_order_id == 91001 && order.sess_id == 321 &&
                            order.isin_id == 1001 && order.dir == 1,
                        "independent surface changed exact order identity");
                require(order.from_trade_repl != order.from_user_book && !order.from_current_day,
                        "independent surface manufactured mixed provenance");
                require(order.public_amount_rest == (order.from_trade_repl ? 0 : 2) &&
                            order.private_amount_rest == (order.from_trade_repl ? 0 : 2) &&
                            order.public_action == (order.from_trade_repl ? 2 : 1) &&
                            order.private_action == (order.from_trade_repl ? 2 : 1),
                        "stale USERORDERBOOK overwrote terminal TRADE fields or lost its separate evidence");
            }
            projector.reset_stream_snapshot(book_first ? kFortsTradeRepl : kFortsUserorderbookRepl);
            require(projector.own_orders().size() == 1 && projector.own_orders()[0].from_trade_repl == !book_first &&
                        projector.own_orders()[0].public_amount_rest == (book_first ? 2 : 0),
                    "one stream reset removed or mutated the other surface");
        }
    }
}

void clone_pending_native_transaction() {
    using Projector = private_state::Plaza2PrivateStateProjector;
    using namespace projection;
    const auto begin = [](Projector& projector, EngineState& state) {
        state.transaction_open = true;
        projector.on_event(
            {}, {.kind = EventKind::kTransactionBegin, .stream_code = kFortsTradeRepl, .numeric_value = 1}, state);
    };
    const auto row = [](Projector& projector, const EngineState& state, std::int64_t id, std::int64_t remaining) {
        const std::array fields{FieldValueSpec{.field_code = kFortsTradeReplOrdersLogPublicOrderId,
                                               .kind = ValueKind::kSignedInteger,
                                               .signed_value = id},
                                FieldValueSpec{.field_code = kFortsTradeReplOrdersLogPrivateOrderId,
                                               .kind = ValueKind::kSignedInteger,
                                               .signed_value = id},
                                FieldValueSpec{.field_code = kFortsTradeReplOrdersLogPublicAmountRest,
                                               .kind = ValueKind::kSignedInteger,
                                               .signed_value = remaining}};
        projector.on_stream_row({},
                                {.kind = EventKind::kStreamData,
                                 .stream_code = kFortsTradeRepl,
                                 .table_code = kFortsTradeReplOrdersLog,
                                 .signed_value = id},
                                {.stream_code = kFortsTradeRepl,
                                 .table_code = kFortsTradeReplOrdersLog,
                                 .field_count = static_cast<std::uint32_t>(fields.size())},
                                fields, state);
    };
    const auto commit = [](Projector& projector, EngineState& state) {
        state.transaction_open = false;
        ++state.commit_count;
        const EventSpec event{.kind = EventKind::kTransactionCommit, .stream_code = kFortsTradeRepl};
        projector.on_event({}, event, state);
        projector.on_transaction_commit({}, event, state);
    };
    Projector clone;
    EngineState cloned_state;
    {
        Projector original;
        EngineState state;
        state.open = true;
        state.streams.push_back({.stream_code = kFortsTradeRepl});
        begin(original, state);
        row(original, state, 77, 7);
        original.on_event({},
                          {.kind = EventKind::kClearDeleted,
                           .stream_code = kFortsTradeRepl,
                           .table_code = kFortsTradeReplOrdersLog,
                           .signed_value = std::numeric_limits<std::int64_t>::max()},
                          state);
        row(original, state, 77, 4);
        row(original, state, 77, 3);
        clone = original.clone();
        cloned_state = state;
        require(clone.own_orders().empty(), "cloning exposed an uncommitted native row");
        original.reset();
        begin(original, state);
        row(original, state, 78, 5);
        commit(original, state);
        require(original.own_orders().size() == 1 && original.own_orders()[0].public_order_id == 78,
                "reset source retained cloned transaction state");
    }
    // The source and its pending storage have now been destroyed. The clone
    // owns the recreated key and publishes its latest row exactly once.
    commit(clone, cloned_state);
    const auto initial = clone.take_row_changes();
    require(clone.own_orders().size() == 1 && initial.orders.size() == 1 && initial.orders[0].public_order_id == 77 &&
                initial.orders[0].public_amount_rest == 3,
            "clone lost or duplicated its pending native row after source destruction");
    begin(clone, cloned_state);
    row(clone, cloned_state, 77, 2);
    commit(clone, cloned_state);
    const auto updated = clone.take_row_changes();
    require(clone.own_orders().size() == 1 && updated.orders.size() == 1 && updated.orders[0].public_order_id == 77 &&
                updated.orders[0].public_amount_rest == 2 && updated.orders[0].trade_repl_commit_sequence == 2,
            "cloned transaction storage was not independent across commits");
}

void duplicate_native_trade_changes() {
    private_state::Plaza2PrivateStateProjector projector;
    Plaza2PrivateStateBridge bridge(projector);
    const std::array streams{kFortsTradeRepl};
    require(!bridge.reset(streams) && !bridge.begin_run(), "duplicate trades begin run");
    const auto event = [&](Plaza2ListenerEventKind kind) {
        require(!bridge.on_plaza2_listener_event({.kind = kind, .stream_code = kFortsTradeRepl}),
                "duplicate trades transaction event");
    };
    std::array fields{Plaza2DecodedFieldValue{.field_code = kFortsTradeReplUserDealIdDeal,
                                              .kind = Plaza2DecodedValueKind::SignedInteger},
                      Plaza2DecodedFieldValue{.field_code = kFortsTradeReplUserDealXamount,
                                              .kind = Plaza2DecodedValueKind::SignedInteger}};
    std::int64_t revision{};
    const auto row = [&](std::int64_t id, std::int64_t amount) {
        fields[0].signed_value = id;
        fields[1].signed_value = amount;
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplUserDeal,
                                                  .fields = fields,
                                                  .signed_value = ++revision}),
                "duplicate trade row");
    };
    event(Plaza2ListenerEventKind::TransactionBegin);
    row(3, 1);
    row(1, 2);
    row(3, 3);
    row(2, 4);
    row(1, 5);
    require(projector.take_row_changes().trades.empty(), "pending duplicate trades became visible");
    event(Plaza2ListenerEventKind::TransactionCommit);
    const auto initial = projector.take_row_changes();
    require(projector.own_trades().size() == 3 && initial.trades.size() == 3,
            "duplicate trade keys did not publish one final row per identity");
    const auto amount = [&](std::int64_t id) {
        const auto found = std::find_if(initial.trades.begin(), initial.trades.end(),
                                        [id](const auto& trade) { return trade.id_deal == id; });
        return found == initial.trades.end() ? 0 : found->amount;
    };
    require(amount(1) == 5 && amount(2) == 4 && amount(3) == 3, "duplicate trade delta lost latest amounts");
    event(Plaza2ListenerEventKind::TransactionBegin);
    row(1, 7);
    row(1, 8);
    event(Plaza2ListenerEventKind::TransactionCommit);
    const auto updated = projector.take_row_changes();
    require(projector.own_trades().size() == 3 && updated.trades.size() == 1 && updated.trades[0].id_deal == 1 &&
                updated.trades[0].amount == 8,
            "online duplicate trade touches changed identity or emitted intermediate rows");

    std::array order_fields{Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicOrderId,
                                                    .kind = Plaza2DecodedValueKind::SignedInteger,
                                                    .signed_value = 88},
                            Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPrivateOrderId,
                                                    .kind = Plaza2DecodedValueKind::SignedInteger,
                                                    .signed_value = 88},
                            Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicAmountRest,
                                                    .kind = Plaza2DecodedValueKind::SignedInteger}};
    const auto order_row = [&](std::int64_t remaining) {
        order_fields[2].signed_value = remaining;
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .fields = order_fields,
                                                  .signed_value = ++revision}),
                "bounded delta order row");
    };
    event(Plaza2ListenerEventKind::TransactionBegin);
    order_row(5);
    event(Plaza2ListenerEventKind::TransactionCommit);
    require(projector.take_row_changes().orders.size() == 1, "bounded delta initial working order");
    const auto fill_queue = [&](std::int64_t first_id) {
        for (std::size_t index = 0; index < private_state::kPrivateRowChangeCapacity; ++index) {
            event(Plaza2ListenerEventKind::TransactionBegin);
            row(first_id + static_cast<std::int64_t>(index), 1);
            event(Plaza2ListenerEventKind::TransactionCommit);
        }
    };
    fill_queue(100);
    const auto boundary = projector.take_row_changes();
    require(boundary.trades.size() == private_state::kPrivateRowChangeCapacity && boundary.orders.empty() &&
                !boundary.resync_required,
            "delta at the exact combined capacity is incomplete or spuriously requests resync");
    fill_queue(20000);
    event(Plaza2ListenerEventKind::TransactionBegin);
    order_row(0);
    row(30000, 3);
    event(Plaza2ListenerEventKind::TransactionCommit);
    event(Plaza2ListenerEventKind::TransactionBegin);
    row(30001, 4);
    event(Plaza2ListenerEventKind::TransactionCommit);
    {
        auto pending_purge = projector.clone();
        auto state = projection::EngineState{
            .open = true, .transaction_open = true, .commit_count = projector.connector_health().commit_count};
        pending_purge.on_event(
            {}, {.kind = projection::EventKind::kTransactionBegin, .stream_code = kFortsTradeRepl, .numeric_value = 1},
            state);
        pending_purge.on_event({},
                               {.kind = projection::EventKind::kClearDeleted,
                                .stream_code = kFortsTradeRepl,
                                .table_code = kFortsTradeReplUserDeal,
                                .signed_value = std::numeric_limits<std::int64_t>::max()},
                               state);
        const auto before_commit = pending_purge.clone().take_row_changes();
        require(before_commit.resync_required && before_commit.regular_trade_history_truncated &&
                    pending_purge.own_trades().size() == projector.own_trades().size(),
                "uncommitted private purge changed the committed snapshot or overflow marker");
        state.transaction_open = false;
        ++state.commit_count;
        pending_purge.on_transaction_commit(
            {}, {.kind = projection::EventKind::kTransactionCommit, .stream_code = kFortsTradeRepl}, state);
        const auto committed_purge = pending_purge.take_row_changes();
        require(committed_purge.resync_required && committed_purge.regular_trade_history_truncated &&
                    pending_purge.own_trades().empty(),
                "committed private purge did not preserve snapshot resync");
    }
    const auto overflow = projector.take_row_changes();
    require(overflow.resync_required && overflow.trade_history_truncated && overflow.regular_trade_history_truncated &&
                overflow.orders.empty() && overflow.trades.empty(),
            "overflow must stay sticky across commits and return no partial cancellation or fill batch");
    const auto latest_fill = std::find_if(projector.own_trades().begin(), projector.own_trades().end(),
                                          [](const auto& trade) { return trade.id_deal == 30001; });
    require(projector.own_orders().size() == 1 && projector.own_orders()[0].public_amount_rest == 0 &&
                projector.own_trades().size() == 2 * private_state::kPrivateRowChangeCapacity + 5 &&
                latest_fill != projector.own_trades().end() && latest_fill->amount == 4,
            "overflow lost committed cancellation or individual fill records needed for snapshot resync");
    fill_queue(40000);
    event(Plaza2ListenerEventKind::TransactionBegin);
    row(50000, 6);
    event(Plaza2ListenerEventKind::TransactionCommit);
    require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                              .stream_code = kFortsTradeRepl,
                                              .table_code = kFortsTradeReplUserDeal,
                                              .signed_value = std::numeric_limits<std::int64_t>::max()}),
            "retire private history after overflow");
    event(Plaza2ListenerEventKind::Online);
    auto closed = projector.clone();
    closed.on_event({}, {.kind = projection::EventKind::kClose}, {});
    const auto closed_changes = closed.take_row_changes();
    require(closed_changes.resync_required && closed_changes.trade_history_truncated &&
                closed_changes.regular_trade_history_truncated,
            "global listener invalidation lost prior truncation or snapshot resync");
    const auto lost = projector.take_row_changes();
    require(lost.resync_required && lost.orders.empty() && lost.trades.empty() && projector.own_trades().empty(),
            "retired undrained fills must require current snapshot resync without partial deltas");
    event(Plaza2ListenerEventKind::TransactionBegin);
    row(60000, 1);
    event(Plaza2ListenerEventKind::TransactionCommit);
    const auto resumed = projector.take_row_changes();
    require(!resumed.resync_required && !resumed.trade_history_truncated && !resumed.regular_trade_history_truncated &&
                resumed.trades.size() == 1 && resumed.trades[0].id_deal == 60000,
            "acknowledging snapshot recovery did not restore bounded delta delivery");
    const auto purge_trades = [&] {
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplUserDeal,
                                                  .signed_value = std::numeric_limits<std::int64_t>::max()}),
                "under-cap private trade purge");
        event(Plaza2ListenerEventKind::Online);
    };
    purge_trades();
    const auto drained_purge = projector.take_row_changes();
    require(!drained_purge.resync_required && drained_purge.trade_history_truncated &&
                drained_purge.regular_trade_history_truncated,
            "drained TRADE purge must report truncated fill history without inventing a missing delta");
    event(Plaza2ListenerEventKind::TransactionBegin);
    row(60001, 2);
    event(Plaza2ListenerEventKind::TransactionCommit);
    auto trade_close = projector.clone();
    trade_close.on_event({}, {.kind = projection::EventKind::kClose, .stream_code = kFortsTradeRepl}, {});
    const auto close_gap = trade_close.take_row_changes();
    require(close_gap.resync_required && !close_gap.trade_history_truncated && close_gap.trades.empty(),
            "listener close silently discarded an under-cap committed fill");
    auto pending_clear = projector.clone();
    (void)pending_clear.take_row_changes();
    projection::EngineState pending_state{.open = true, .transaction_open = true};
    pending_clear.on_event(
        {}, {.kind = projection::EventKind::kTransactionBegin, .stream_code = kFortsTradeRepl, .numeric_value = 1},
        pending_state);
    pending_clear.on_event({},
                           {.kind = projection::EventKind::kClearDeleted,
                            .stream_code = kFortsTradeRepl,
                            .table_code = kFortsTradeReplUserDeal,
                            .signed_value = std::numeric_limits<std::int64_t>::max()},
                           pending_state);
    const auto uncommitted_clear = pending_clear.clone().take_row_changes();
    require(!uncommitted_clear.trade_history_truncated && !uncommitted_clear.regular_trade_history_truncated,
            "uncommitted ClearDeleted invalidated the fill baseline");
    pending_state.transaction_open = false;
    ++pending_state.commit_count;
    pending_clear.on_transaction_commit(
        {}, {.kind = projection::EventKind::kTransactionCommit, .stream_code = kFortsTradeRepl}, pending_state);
    const auto committed_clear = pending_clear.take_row_changes();
    require(committed_clear.trade_history_truncated && committed_clear.regular_trade_history_truncated,
            "committed ClearDeleted omitted fill-history truncation");
    auto changed_life = projector.clone();
    (void)changed_life.take_row_changes();
    changed_life.on_event(
        {}, {.kind = projection::EventKind::kLifeNum, .stream_code = kFortsTradeRepl, .numeric_value = 7}, {});
    require(!changed_life.take_row_changes().trade_history_truncated,
            "first known TRADE LifeNum was treated as a history change");
    changed_life.on_event(
        {}, {.kind = projection::EventKind::kLifeNum, .stream_code = kFortsTradeRepl, .numeric_value = 8}, {});
    const auto life_loss = changed_life.take_row_changes();
    require(life_loss.trade_history_truncated && life_loss.regular_trade_history_truncated,
            "changed TRADE LifeNum omitted fill-history truncation");
    purge_trades();
    const auto purge_gap = projector.take_row_changes();
    require(purge_gap.resync_required && purge_gap.trade_history_truncated && purge_gap.trades.empty(),
            "technical purge silently discarded an under-cap committed fill");
}

void committed_trade_purge_floors() {
    using namespace projection;
    private_state::Plaza2PrivateStateProjector projector;
    EngineState state{.open = true};
    const auto begin = [&] {
        state.transaction_open = true;
        projector.on_event(
            {}, {.kind = EventKind::kTransactionBegin, .stream_code = kFortsTradeRepl, .numeric_value = 1}, state);
    };
    const auto commit = [&] {
        state.transaction_open = false;
        ++state.commit_count;
        projector.on_transaction_commit({}, {.kind = EventKind::kTransactionCommit, .stream_code = kFortsTradeRepl},
                                        state);
    };
    const auto clear = [&](generated::TableCode table, std::int64_t revision) {
        projector.on_event({},
                           {.kind = EventKind::kClearDeleted,
                            .stream_code = kFortsTradeRepl,
                            .table_code = table,
                            .signed_value = revision},
                           state);
    };
    const std::array tables{kFortsTradeReplOrdersLog, kFortsTradeReplMultilegOrdersLog, kFortsTradeReplUserDeal,
                            kFortsTradeReplUserMultilegDeal};
    begin();
    for (const auto table : tables) {
        clear(table, 100);
        clear(table, 200);
    }
    commit();
    require(projector.take_row_changes().trade_history_truncated,
            "first positive purge floors must conservatively report possible unseen history loss");
    projector.on_event({}, {.kind = EventKind::kClose, .stream_code = kFortsTradeRepl}, state);
    projector.reset_stream_snapshot(kFortsTradeRepl);
    begin();
    for (const auto table : tables) {
        clear(table, 100);
        clear(table, 200);
        clear(table, 199);
    }
    clear(kFortsTradeReplHeartbeat, 1000);
    clear(kFortsTradeReplSysEvents, 1000);
    commit();
    require(!projector.take_row_changes().trade_history_truncated,
            "unchanged positive reopen markers without retired own rows invalidated execution history");
    begin();
    clear(kFortsTradeReplUserDeal, 300);
    require(!projector.clone().take_row_changes().trade_history_truncated,
            "an uncommitted purge floor became externally visible");
    begin(); // Abort the previous transaction, then replay its marker.
    clear(kFortsTradeReplUserDeal, 300);
    commit();
    require(projector.own_trades().empty() && projector.take_row_changes().trade_history_truncated,
            "advanced purge floor with no cached own trades lost its signal after transaction rollback");
    begin();
    clear(kFortsTradeReplUserDeal, 300);
    clear(kFortsTradeReplUserMultilegDeal, 200);
    commit();
    require(!projector.take_row_changes().trade_history_truncated,
            "committed purge floors were not tracked independently per table");
    begin();
    clear(kFortsTradeReplUserDeal, std::numeric_limits<std::int64_t>::max());
    begin(); // An aborted clear-all must not reset the committed finite floor.
    clear(kFortsTradeReplUserDeal, 300);
    commit();
    require(!projector.take_row_changes().trade_history_truncated,
            "aborted clear-all reset a committed finite purge floor");
    for (const auto table :
         {kFortsTradeReplOrdersLog, kFortsTradeReplMultilegOrdersLog, kFortsTradeReplUserMultilegDeal}) {
        begin();
        clear(table, 201);
        commit();
        const auto scoped_loss = projector.take_row_changes();
        require(scoped_loss.trade_history_truncated &&
                    scoped_loss.regular_trade_history_truncated == (table == kFortsTradeReplOrdersLog),
                "a different TRADE table's larger floor masked this table's advancing floor");
    }
    for (const auto table : {kFortsTradeReplMultilegOrdersLog, kFortsTradeReplUserMultilegDeal}) {
        begin();
        clear(table, std::numeric_limits<std::int64_t>::max());
        commit();
        const auto multileg_loss = projector.take_row_changes();
        require(multileg_loss.trade_history_truncated && !multileg_loss.regular_trade_history_truncated,
                "multileg clear-all erased the regular-order baseline scope or omitted broad projection loss");
    }
    begin();
    const std::array fields{FieldValueSpec{
        .field_code = kFortsTradeReplUserDealIdDeal, .kind = ValueKind::kSignedInteger, .signed_value = 20}};
    projector.on_stream_row({},
                            {.kind = EventKind::kStreamData,
                             .stream_code = kFortsTradeRepl,
                             .table_code = kFortsTradeReplUserDeal,
                             .signed_value = 150},
                            {.stream_code = kFortsTradeRepl,
                             .table_code = kFortsTradeReplUserDeal,
                             .field_count = static_cast<std::uint32_t>(fields.size())},
                            fields, state);
    commit();
    require(projector.own_trades().size() == 1, "purge-floor retirement fixture omitted its owned trade");
    (void)projector.take_row_changes();
    begin();
    clear(kFortsTradeReplUserDeal, 300);
    commit();
    require(projector.own_trades().empty() && projector.take_row_changes().trade_history_truncated,
            "an unchanged purge floor that actually retired a cached own trade lost its history signal");
    begin();
    const std::array order_fields{FieldValueSpec{
        .field_code = kFortsTradeReplOrdersLogPublicOrderId, .kind = ValueKind::kSignedInteger, .signed_value = 30}};
    projector.on_stream_row({},
                            {.kind = EventKind::kStreamData,
                             .stream_code = kFortsTradeRepl,
                             .table_code = kFortsTradeReplOrdersLog,
                             .signed_value = 150},
                            {.stream_code = kFortsTradeRepl,
                             .table_code = kFortsTradeReplOrdersLog,
                             .field_count = static_cast<std::uint32_t>(order_fields.size())},
                            order_fields, state);
    commit();
    require(projector.own_orders().size() == 1, "purge-floor retirement fixture omitted its owned order");
    (void)projector.take_row_changes();
    begin();
    clear(kFortsTradeReplOrdersLog, 201);
    commit();
    require(projector.own_orders().empty() && projector.take_row_changes().trade_history_truncated,
            "an unchanged purge floor that actually retired a cached own order lost its history signal");
    projector.on_event({}, {.kind = EventKind::kLifeNum, .stream_code = kFortsTradeRepl, .numeric_value = 7}, state);
    projector.on_event({}, {.kind = EventKind::kLifeNum, .stream_code = kFortsTradeRepl, .numeric_value = 8}, state);
    require(projector.take_row_changes().trade_history_truncated, "changed TRADE epoch omitted history loss");
    begin();
    clear(kFortsTradeReplUserDeal, 100);
    commit();
    require(projector.take_row_changes().trade_history_truncated,
            "new TRADE epoch inherited the prior epoch's purge floor");
    clear(projection::kNoTableCode, 0);
    (void)projector.take_row_changes();
    begin();
    clear(kFortsTradeReplUserDeal, 100);
    commit();
    require(projector.take_row_changes().trade_history_truncated, "whole TRADE reset inherited a prior purge floor");
    begin();
    clear(kFortsTradeReplUserDeal, std::numeric_limits<std::int64_t>::max());
    commit();
    require(projector.take_row_changes().trade_history_truncated, "clear-all omitted possible fill-history loss");
    begin();
    clear(kFortsTradeReplUserDeal, std::numeric_limits<std::int64_t>::max());
    commit();
    require(projector.take_row_changes().trade_history_truncated,
            "repeated clear-all with an empty owned trade cache omitted possible fill-history loss");
    projector.reset_stream_snapshot(kFortsTradeRepl);
    begin();
    projector.on_stream_row({},
                            {.kind = EventKind::kStreamData,
                             .stream_code = kFortsTradeRepl,
                             .table_code = kFortsTradeReplOrdersLog,
                             .signed_value = 400},
                            {.stream_code = kFortsTradeRepl,
                             .table_code = kFortsTradeReplOrdersLog,
                             .field_count = static_cast<std::uint32_t>(order_fields.size())},
                            order_fields, state);
    commit();
    require(projector.own_orders().size() == 1 && !projector.take_row_changes().trade_history_truncated,
            "same-LifeNum rebuilt working order inherited the earlier clear-all's history loss");
    projector.on_event({}, {.kind = EventKind::kClose, .stream_code = kFortsTradeRepl}, state);
    projector.reset_stream_snapshot(kFortsTradeRepl);
    (void)projector.take_row_changes();
    begin();
    clear(kFortsTradeReplUserDeal, 200);
    commit();
    require(projector.own_trades().empty() && projector.take_row_changes().trade_history_truncated,
            "clear-all watermark hid a later finite purge floor with no cached own trades");
}

std::chrono::nanoseconds snapshot_median(std::array<std::chrono::nanoseconds, 3> samples) {
    std::sort(samples.begin(), samples.end());
    return samples[1];
}

bool snapshot_release_accepted(const std::array<std::chrono::nanoseconds, 3>& samples) {
    return snapshot_median(samples) <= std::chrono::seconds(2);
}

void snapshot_acceptance_boundaries() {
    using namespace std::chrono_literals;
    require(snapshot_release_accepted({1141ms, 1189ms, 1138ms}),
            "the measured deployment-host snapshot median must satisfy the approved 2s gate");
    require(snapshot_release_accepted({1999ms, 2s, 9s}), "snapshot acceptance must include the exact 2s median");
    require(!snapshot_release_accepted({2s + 1ns, 9s, 2s + 2ns}), "snapshot acceptance must reject a median above 2s");
    require(snapshot_release_accepted({9s, 1s, 1999ms}), "one slow snapshot must not replace the three-run median");
    require(!snapshot_release_accepted({1s, 2001ms, 2002ms}), "two slow snapshots must fail the median gate");
}

int transaction_scenario(std::chrono::nanoseconds& snapshot_duration) {
    try {
        private_state::Plaza2PrivateStateProjector projector;
        Plaza2PrivateStateBridge bridge(projector);
        std::array streams{kFortsTradeRepl, kFortsPosRepl};
        require(!bridge.reset(streams), "reset");
        require(!bridge.begin_run(), "begin run");
        const auto event = [&](Plaza2ListenerEventKind kind, generated::StreamCode stream) {
            require(!bridge.on_plaza2_listener_event({.kind = kind, .stream_code = stream}), "event rejected");
        };
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsPosRepl);
        std::array position{Plaza2DecodedFieldValue{.field_code = kFortsPosReplPositionIsinId,
                                                    .kind = Plaza2DecodedValueKind::SignedInteger,
                                                    .signed_value = 1001},
                            Plaza2DecodedFieldValue{.field_code = kFortsPosReplPositionXpos,
                                                    .kind = Plaza2DecodedValueKind::SignedInteger,
                                                    .signed_value = 3}};
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                  .stream_code = kFortsPosRepl,
                                                  .table_code = kFortsPosReplPosition,
                                                  .fields = position}),
                "POS row");
        auto start = std::chrono::steady_clock::now();
        std::array fields{Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicOrderId,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPrivateOrderId,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogIsinId,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger,
                                                  .signed_value = 1001},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicAmount,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger,
                                                  .signed_value = 5},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicAmountRest,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger,
                                                  .signed_value = 5},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogIdOrd1,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger}};
        for (std::int64_t id = 1; id <= 150000; ++id) {
            fields[0].signed_value = id;
            fields[1].signed_value = id;
            require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                      .stream_code = kFortsTradeRepl,
                                                      .table_code = kFortsTradeReplOrdersLog,
                                                      .fields = fields,
                                                      .signed_value = id}),
                    "TRADE row");
        }
        require(projector.own_orders().empty() && projector.positions().empty(), "uncommitted rows became visible");
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        snapshot_duration = std::chrono::steady_clock::now() - start;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(snapshot_duration).count();
        require(projector.own_orders().size() == 150000, "order index lost rows");
        require(projector.storage_capacity().order_snapshot_capacity <= 200000,
                "150k order snapshot retains excessive reserve capacity");
        const auto initial_orders = projector.own_orders();
        require(
            initial_orders.front().public_order_id == 1 && initial_orders.front().private_order_id == 1 &&
                initial_orders.back().public_order_id == 150000 && initial_orders.back().private_order_id == 150000 &&
                std::all_of(initial_orders.begin(), initial_orders.end(),
                            [](const auto& order) {
                                return order.public_order_id_aliases.empty() && order.private_order_id_aliases.empty();
                            }),
            "native canonical IDs were lost or duplicated into heap-allocated aliases");
        require(projector.positions().empty(), "TRADE commit leaked open POS transaction");
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsPosRepl);
        require(projector.positions().size() == 1 && projector.positions()[0].xpos == 3,
                "POS transaction not independently committed");
        const auto initial_changes = projector.take_row_changes();
        require(initial_changes.orders.size() + initial_changes.trades.size() <=
                    private_state::kPrivateRowChangeCapacity,
                "undrained private delta exceeds its bounded row capacity");
        require(initial_changes.resync_required && initial_changes.orders.empty() && initial_changes.trades.empty(),
                "large committed bootstrap must request full snapshot resync without partial deltas");
        std::array<std::int64_t, 20> update_us{};
        for (std::size_t index = 0; index < update_us.size(); ++index) {
            const auto before = std::chrono::steady_clock::now();
            event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
            fields[4].signed_value = index % 2 ? 3 : 4;
            fields[5].signed_value = 149999;
            require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                      .stream_code = kFortsTradeRepl,
                                                      .table_code = kFortsTradeReplOrdersLog,
                                                      .fields = fields,
                                                      .signed_value = 150001 + static_cast<std::int64_t>(index)}),
                    "single row update");
            require(projector.take_row_changes().orders.empty(), "uncommitted row escaped through delta");
            event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
            const auto orders = projector.own_orders();
            require(orders.size() == 150000 && orders.back().public_order_id == 150000 &&
                        orders.back().public_amount_rest == fields[4].signed_value,
                    "single update changed identity, row count or remaining quantity");
            const auto delta = projector.take_row_changes();
            require(delta.orders.size() == 1 && delta.orders[0].public_order_id == 150000 &&
                        delta.orders[0].public_amount_rest == fields[4].signed_value &&
                        delta.orders[0].id_ord1 == 149999 && delta.orders[0].trade_repl_commit_sequence > 0,
                    "committed delta omitted update or day-switch linkage");
            update_us[index] =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - before)
                    .count();
        }
        auto sorted_updates = update_us;
        std::sort(sorted_updates.begin(), sorted_updates.end());
        std::cout << "150k book single-row update median/p95: " << sorted_updates[10] << "/" << sorted_updates[18]
                  << " us\n";
        const auto capacity_before_insert = projector.storage_capacity();
        const auto* view_before_insert = projector.own_orders().data();
        const auto before_insert = std::chrono::steady_clock::now();
        std::array<std::chrono::steady_clock::time_point, 6> insertion_times;
        insertion_times[0] = before_insert;
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        insertion_times[1] = std::chrono::steady_clock::now();
        fields[0].signed_value = fields[1].signed_value = 150001;
        fields[5].signed_value = 0;
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .fields = fields,
                                                  .signed_value = 150021}),
                "new order after 150k snapshot");
        insertion_times[2] = std::chrono::steady_clock::now();
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        insertion_times[3] = std::chrono::steady_clock::now();
        const auto inserted_orders = projector.own_orders();
        insertion_times[4] = std::chrono::steady_clock::now();
        const auto inserted_changes = projector.take_row_changes();
        insertion_times[5] = std::chrono::steady_clock::now();
        require(inserted_orders.size() == 150001 && inserted_orders.back().public_order_id == 150001 &&
                    inserted_changes.orders.size() == 1 && inserted_changes.orders[0].public_order_id == 150001,
                "new order was not appended to the committed indexed view and delta");
        const auto insert_us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - before_insert)
                .count();
        std::cout << "150k book new-order commit and delta: " << insert_us << " us\n";
        std::cout << "new-order begin/row/commit/view/delta phases: ";
        for (std::size_t index = 1; index < insertion_times.size(); ++index) {
            if (index > 1)
                std::cout << '/';
            std::cout << std::chrono::duration_cast<std::chrono::microseconds>(insertion_times[index] -
                                                                               insertion_times[index - 1])
                             .count();
        }
        std::cout << " us\n";
        const auto capacity_after_insert = projector.storage_capacity();
        const auto print_capacity = [](const auto& capacity) {
            std::cout << capacity.order_buckets << '/' << capacity.order_identity_buckets << '/'
                      << capacity.order_view_buckets << '/' << capacity.source_row_buckets << '/'
                      << capacity.order_snapshot_capacity;
        };
        std::cout << "order/identity/view/revision buckets and snapshot capacity before/after insertion: ";
        print_capacity(capacity_before_insert);
        std::cout << " -> ";
        print_capacity(capacity_after_insert);
        std::cout << '\n';
        require(capacity_before_insert == capacity_after_insert && inserted_orders.data() == view_before_insert,
                "first online insertion rehashed an index or reallocated the committed view");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .fields = fields,
                                                  .signed_value = 150022}),
                "delta queued before purge");
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .signed_value = std::numeric_limits<std::int64_t>::max()}),
                "clear");
        event(Plaza2ListenerEventKind::Online, kFortsTradeRepl);
        require(projector.take_row_changes().orders.empty(), "MAX purge left obsolete committed delta");
        require(projector.own_orders().empty() && projector.positions().size() == 1,
                "ClearDeleted affected wrong domain");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .fields = fields,
                                                  .signed_value = 1}),
                "row after MAX");
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        require(projector.own_orders().size() == 1, "MAX revision reset did not accept new row");
        require(projector.take_row_changes().orders.size() == 1, "row after MAX missing committed delta");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        fields[0].signed_value = 150001;
        fields[1].signed_value = 150001;
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .fields = fields,
                                                  .signed_value = 2}),
                "second fresh row");
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .signed_value = 2}),
                "normal clear");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        require(projector.own_orders().size() == 1 && projector.own_orders()[0].public_order_id == 150001,
                "normal ClearDeleted did not filter rows by revision");
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .stream_code = kFortsPosRepl,
                                                  .table_code = kFortsPosReplPosition,
                                                  .signed_value = std::numeric_limits<std::int64_t>::max()}),
                "POS clear");
        event(Plaza2ListenerEventKind::Online, kFortsPosRepl);
        require(projector.positions().empty() && projector.own_orders().size() == 1, "POS clear crossed streams");
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .signed_value = std::numeric_limits<std::int64_t>::max()}),
                "fresh relist scenario clears previous history");
        event(Plaza2ListenerEventKind::Online, kFortsTradeRepl);
        const auto discarded = projector.take_row_changes();
        (void)discarded;
        std::array relist{Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicOrderId,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger,
                                                  .signed_value = 9001},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPrivateOrderId,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger,
                                                  .signed_value = 9001},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogSessId,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger,
                                                  .signed_value = 321},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogExtId,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger,
                                                  .signed_value = 17},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogClientCode,
                                                  .kind = Plaza2DecodedValueKind::String,
                                                  .text_value = "BRK1C01"},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogPublicAmountRest,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger,
                                                  .signed_value = 5},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogIdOrd1,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger},
                          Plaza2DecodedFieldValue{.field_code = kFortsTradeReplOrdersLogReplRev,
                                                  .kind = Plaza2DecodedValueKind::SignedInteger,
                                                  .signed_value = 1}};
        const auto relist_row = [&] {
            require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                      .stream_code = kFortsTradeRepl,
                                                      .table_code = kFortsTradeReplOrdersLog,
                                                      .fields = relist,
                                                      .signed_value = relist[7].signed_value}),
                    "day relist row");
        };
        // A Move replacement reuses ext_id and can arrive before reply 176.
        // Both raw exchange records must retain their actual IDs for deltas.
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        relist[0].signed_value = relist[1].signed_value = 5001;
        relist_row();
        relist[0].signed_value = relist[1].signed_value = 5002;
        relist[7].signed_value = 2;
        relist_row();
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        const auto moved_rows = projector.take_row_changes();
        require(projector.own_orders().size() == 2 && moved_rows.orders.size() == 2,
                "Move records sharing ext_id were coalesced");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        relist[5].signed_value = 0;
        relist[7].signed_value = 3;
        relist_row();
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        const auto terminal_move = projector.take_row_changes();
        require(terminal_move.orders.size() == 1 && terminal_move.orders[0].public_order_id == 5002 &&
                    terminal_move.orders[0].public_amount_rest == 0 && !terminal_move.orders[0].identity_conflict,
                "Move successor terminal delta lost its new exchange identity");
        // Canonical scalar IDs retain real alternate source identifiers. The
        // next update resolves through the alternate public ID index.
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        relist[0].signed_value = 5101;
        relist[1].signed_value = 5001;
        relist[7].signed_value = 4;
        relist_row();
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        const auto public_alias = projector.take_row_changes();
        require(public_alias.orders.size() == 1 && public_alias.orders[0].public_order_id == 5001 &&
                    public_alias.orders[0].private_order_id == 5001 &&
                    public_alias.orders[0].public_order_id_aliases == std::vector<std::int64_t>{5101} &&
                    public_alias.orders[0].private_order_id_aliases.empty(),
                "real public alias was dropped or canonical ID duplicated");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        relist[1].signed_value = 5102;
        relist[7].signed_value = 5;
        relist_row();
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        const auto both_aliases = projector.take_row_changes();
        require(projector.own_orders().size() == 2 && both_aliases.orders.size() == 1 &&
                    both_aliases.orders[0].public_order_id == 5001 && both_aliases.orders[0].private_order_id == 5001 &&
                    both_aliases.orders[0].public_order_id_aliases == std::vector<std::int64_t>{5101} &&
                    both_aliases.orders[0].private_order_id_aliases == std::vector<std::int64_t>{5102},
                "alternate ID index stopped resolving the committed native order");
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .signed_value = std::numeric_limits<std::int64_t>::max()}),
                "clear Move records before day-link scenario");
        event(Plaza2ListenerEventKind::Online, kFortsTradeRepl);
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        relist[0].signed_value = relist[1].signed_value = 9001;
        relist[5].signed_value = 5;
        relist[7].signed_value = 1;
        relist_row();
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        require(projector.take_row_changes().orders.size() == 1, "initial relist order delta");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        relist[5].signed_value = 0;
        relist_row();
        relist[0].signed_value = relist[1].signed_value = 9002;
        relist[2].signed_value = 322;
        relist[5].signed_value = 5;
        relist[6].signed_value = 9001;
        relist[7].signed_value = 2;
        relist_row();
        require(projector.take_row_changes().orders.empty(), "relist exposed before transaction commit");
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        const auto linked = projector.take_row_changes();
        require(linked.orders.size() == 2 && projector.own_orders().size() == 2 &&
                    linked.orders[0].trade_repl_commit_sequence == linked.orders[1].trade_repl_commit_sequence,
                "relist delete/new ID must publish as one transaction batch");
        const auto new_row = std::find_if(linked.orders.begin(), linked.orders.end(),
                                          [](const auto& row) { return row.public_order_id == 9002; });
        require(new_row != linked.orders.end() && new_row->id_ord1 == 9001 && !new_row->identity_conflict &&
                    new_row->public_amount_rest == 5,
                "day linkage was coalesced by ext_id or omitted from committed row");
        // Queue both identities again, then purge only the old revision. The
        // shared ext_id must not retain an obsolete previous-day delta.
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        relist[0].signed_value = relist[1].signed_value = 9001;
        relist[2].signed_value = 321;
        relist[5].signed_value = 0;
        relist[6].signed_value = 0;
        relist[7].signed_value = 1;
        relist_row();
        relist[0].signed_value = relist[1].signed_value = 9002;
        relist[2].signed_value = 322;
        relist[5].signed_value = 5;
        relist[6].signed_value = 9001;
        relist[7].signed_value = 2;
        relist_row();
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        require(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                  .stream_code = kFortsTradeRepl,
                                                  .table_code = kFortsTradeReplOrdersLog,
                                                  .signed_value = 2}),
                "normal clear prunes old linked identity");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        const auto retained = projector.take_row_changes();
        require(projector.own_orders().size() == 1 && projector.own_orders()[0].public_order_id == 9002 &&
                    retained.resync_required && retained.orders.empty(),
                "purged unconsumed previous-day evidence must require resync without partial deltas");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        relist_row();
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        event(Plaza2ListenerEventKind::Close, kFortsTradeRepl);
        const auto closed_changes = projector.take_row_changes();
        require(closed_changes.orders.empty() && closed_changes.resync_required,
                "listener invalidation silently discarded unconsumed order evidence");
        bridge_owns_callback_values();
        clone_pending_native_transaction();
        duplicate_native_trade_changes();
        std::cout << "150000 TRADE rows including commit: " << elapsed << " ms\n";
#if MOEX_RELEASE_PERFORMANCE_ACCEPTANCE
        require(sorted_updates.back() < 1000, "single-row committed update exceeds Release 1ms acceptance");
        require(insert_us < 1000, "single-order insertion exceeds Release 1ms acceptance");
#endif
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

int main() {
    try {
        terminal_trade_preserves_independent_userbook();
        snapshot_acceptance_boundaries();
        committed_trade_purge_floors();
        std::array<std::chrono::nanoseconds, 3> samples;
        for (auto& sample : samples)
            if (transaction_scenario(sample) != 0)
                return 1;
        std::cout << "150k fresh TRADE snapshot runs: ";
        for (const auto sample : samples)
            std::cout << std::chrono::duration<double, std::milli>(sample).count() << " ms ";
        std::cout << "; median: " << std::chrono::duration<double, std::milli>(snapshot_median(samples)).count()
                  << " ms\n";
// Instrumented builds remain diagnostic; Linux Release enforces snapshot capacity.
#if defined(__linux__) && MOEX_RELEASE_PERFORMANCE_ACCEPTANCE
        require(snapshot_release_accepted(samples), "150k TRADE snapshot median exceeds Linux Release 2s acceptance");
#endif
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
