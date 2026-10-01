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
    // The source and its transaction pool have now been destroyed. The clone
    // must publish its pending transaction and reuse its own pool afterward.
    commit(clone, cloned_state);
    const auto initial = clone.take_row_changes();
    require(clone.own_orders().size() == 1 && initial.orders.size() == 1 && initial.orders[0].public_order_id == 77 &&
                initial.orders[0].public_amount_rest == 3,
            "clone lost its pending native rows when source pool was destroyed");
    begin(clone, cloned_state);
    row(clone, cloned_state, 77, 2);
    commit(clone, cloned_state);
    const auto updated = clone.take_row_changes();
    require(clone.own_orders().size() == 1 && updated.orders.size() == 1 && updated.orders[0].public_order_id == 77 &&
                updated.orders[0].public_amount_rest == 2 && updated.orders[0].trade_repl_commit_sequence == 2,
            "cloned transaction pool was not retained across commits");
}

int main() {
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
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        require(projector.own_orders().size() == 150000, "order index lost rows");
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
        require(projector.take_row_changes().orders.size() == 150000, "initial committed upserts missing from delta");
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
        require(projector.own_orders().size() == 1 && retained.orders.size() == 1 &&
                    retained.orders[0].public_order_id == 9002,
                "ext_id reuse retained a purged previous-day delta");
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsTradeRepl);
        relist_row();
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsTradeRepl);
        event(Plaza2ListenerEventKind::Close, kFortsTradeRepl);
        require(projector.take_row_changes().orders.empty(), "listener invalidation left stale row changes");
        clone_pending_native_transaction();
        std::cout << "150000 TRADE rows including commit: " << elapsed << " ms\n";
// Optimized sanitizer builds also define NDEBUG. Their instrumentation overhead
// is diagnostic; the Linux Release job enforces the unchanged capacity limit.
#if defined(__linux__) && MOEX_RELEASE_PERFORMANCE_ACCEPTANCE
        require(elapsed < 1000, "150k TRADE snapshot exceeds Linux Release 1 second acceptance");
#endif
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
