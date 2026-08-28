#include "moex/plaza2/cgate/plaza2_private_state.hpp"

#include "plaza2_fake_scenarios.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using moex::plaza2::fake::EngineState;
using moex::plaza2::fake::EventKind;
using moex::plaza2::fake::EventSpec;
using moex::plaza2::fake::FieldValueSpec;
using moex::plaza2::fake::FindScenarioById;
using moex::plaza2::fake::Plaza2FakeEngine;
using moex::plaza2::fake::RowSpec;
using moex::plaza2::fake::ValueKind;
using moex::plaza2::fake::ViewForScenario;
using moex::plaza2::generated::FieldCode;
using moex::plaza2::generated::StreamCode;
using moex::plaza2::generated::TableCode;
using moex::plaza2::private_state::InstrumentSnapshot;
using moex::plaza2::private_state::OwnOrderSnapshot;
using moex::plaza2::private_state::Plaza2PrivateStateProjector;
using moex::plaza2::private_state::StreamHealthSnapshot;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const OwnOrderSnapshot* find_order(std::span<const OwnOrderSnapshot> orders, std::int64_t private_order_id) {
    for (const auto& order : orders) {
        if (order.private_order_id == private_order_id) {
            return &order;
        }
    }
    return nullptr;
}

const StreamHealthSnapshot* find_stream(std::span<const StreamHealthSnapshot> streams, StreamCode stream_code) {
    for (const auto& stream : streams) {
        if (stream.stream_code == stream_code) {
            return &stream;
        }
    }
    return nullptr;
}

const InstrumentSnapshot* find_instrument(std::span<const InstrumentSnapshot> instruments, std::int32_t isin_id) {
    for (const auto& instrument : instruments) {
        if (instrument.isin_id == isin_id) {
            return &instrument;
        }
    }
    return nullptr;
}

void run_or_throw(const char* scenario_id, Plaza2PrivateStateProjector* projector) {
    const auto* scenario = FindScenarioById(scenario_id);
    require(scenario != nullptr, "required Phase 3E scenario is missing");

    Plaza2FakeEngine engine;
    const auto result = engine.run(ViewForScenario(*scenario), projector);
    if (result.error) {
        throw std::runtime_error(std::string(scenario_id) + " replay failed: " + result.error.message);
    }
}

const StreamHealthSnapshot& require_userbook_health(const Plaza2PrivateStateProjector& projector) {
    const auto* health = find_stream(projector.stream_health(), StreamCode::kFortsUserorderbookRepl);
    require(health != nullptr, "USERORDERBOOK stream health should exist");
    return *health;
}

void commit_userbook_info(Plaza2PrivateStateProjector& projector, EngineState& state, std::int32_t publication_state,
                          bool current_day) {
    const auto table_code =
        current_day ? TableCode::kFortsUserorderbookReplInfoCurrentday : TableCode::kFortsUserorderbookReplInfo;
    const auto publication_field = current_day ? FieldCode::kFortsUserorderbookReplInfoCurrentdayPublicationState
                                               : FieldCode::kFortsUserorderbookReplInfoPublicationState;
    const auto trades_rev_field = current_day ? FieldCode::kFortsUserorderbookReplInfoCurrentdayTradesRev
                                              : FieldCode::kFortsUserorderbookReplInfoTradesRev;
    const auto trades_lifenum_field = current_day ? FieldCode::kFortsUserorderbookReplInfoCurrentdayTradesLifenum
                                                  : FieldCode::kFortsUserorderbookReplInfoTradesLifenum;
    const auto moment_or_server_field = current_day ? FieldCode::kFortsUserorderbookReplInfoCurrentdayServerTime
                                                    : FieldCode::kFortsUserorderbookReplInfoMoment;
    const std::vector<FieldValueSpec> fields = {
        {.field_code = publication_field, .kind = ValueKind::kSignedInteger, .signed_value = publication_state},
        {.field_code = trades_rev_field, .kind = ValueKind::kSignedInteger, .signed_value = 42},
        {.field_code = trades_lifenum_field, .kind = ValueKind::kSignedInteger, .signed_value = 7},
        {.field_code = moment_or_server_field, .kind = ValueKind::kSignedInteger, .signed_value = 1700000042},
    };
    state.transaction_open = true;
    state.streams.front().online = true;
    state.streams.front().snapshot_complete = true;
    const EventSpec begin{.kind = EventKind::kTransactionBegin, .stream_code = StreamCode::kFortsUserorderbookRepl};
    projector.on_event({}, begin, state);
    const EventSpec data{.kind = EventKind::kStreamData,
                         .stream_code = StreamCode::kFortsUserorderbookRepl,
                         .table_code = table_code,
                         .signed_value = 42};
    const RowSpec row{.stream_code = StreamCode::kFortsUserorderbookRepl,
                      .table_code = table_code,
                      .field_count = static_cast<std::uint32_t>(fields.size())};
    projector.on_stream_row({}, data, row, fields, state);
    state.transaction_open = false;
    state.commit_count += 1;
    state.streams.front().committed_row_count += 1;
    const EventSpec commit{.kind = EventKind::kTransactionCommit, .stream_code = StreamCode::kFortsUserorderbookRepl};
    projector.on_event({}, commit, state);
    projector.on_transaction_commit({}, commit, state);
}

void clear_userbook_table(Plaza2PrivateStateProjector& projector, EngineState& state, TableCode table_code) {
    const EventSpec clear{.kind = EventKind::kClearDeleted,
                          .stream_code = StreamCode::kFortsUserorderbookRepl,
                          .table_code = table_code,
                          .signed_value = 42};
    projector.on_event({}, clear, state);
}

} // namespace

int main() {
    try {
        Plaza2PrivateStateProjector clear_deleted_projector;
        run_or_throw("private_state_clear_deleted", &clear_deleted_projector);

        const auto clear_deleted_orders = clear_deleted_projector.own_orders();
        require(clear_deleted_orders.size() == 2, "clear-deleted should leave only trade-owned orders committed");

        const auto* removed_order = find_order(clear_deleted_orders, 21001);
        require(removed_order == nullptr, "user-orderbook-only order should be removed by clear-deleted");

        const auto* trade_only = find_order(clear_deleted_orders, 21002);
        require(trade_only != nullptr, "trade-only order should survive clear-deleted");
        require(trade_only->from_trade_repl, "trade-only order should retain trade source");
        require(!trade_only->from_user_book && !trade_only->from_current_day,
                "trade-only order should not pick up user-orderbook sources");

        const auto* shared = find_order(clear_deleted_orders, 21003);
        require(shared != nullptr, "shared order should survive clear-deleted");
        require(shared->from_trade_repl, "shared order should retain trade source");
        require(!shared->from_user_book && !shared->from_current_day,
                "clear-deleted should clear only user-orderbook ownership");
        require(shared->price == "103500", "shared order should keep the committed trade-side delta");

        const auto* clear_deleted_stream =
            find_stream(clear_deleted_projector.stream_health(), StreamCode::kFortsUserorderbookRepl);
        require(clear_deleted_stream != nullptr, "user-orderbook stream health should exist after clear-deleted");
        require(clear_deleted_stream->clear_deleted_count == 1, "clear-deleted count should advance");
        require(clear_deleted_stream->committed_row_count == 0, "user-orderbook row watermark should be reset");

        Plaza2PrivateStateProjector table_clear_projector;
        run_or_throw("private_state_table_clear_deleted", &table_clear_projector);
        require(table_clear_projector.instruments().size() == 2,
                "table clear should remove stale rows and retain the rebuilt/refdata rows");
        require(find_instrument(table_clear_projector.instruments(), 7001) != nullptr,
                "a fresh transaction should rebuild a previously cleared fut_sess_contents row");
        require(find_instrument(table_clear_projector.instruments(), 7002) != nullptr,
                "fut_sess_contents rows at the clear revision must remain");
        require(find_instrument(table_clear_projector.instruments(), 8001) == nullptr &&
                    find_instrument(table_clear_projector.instruments(), 8002) == nullptr,
                "MAX clear must affect only the addressed fut_instruments table");

        EngineState uncommitted_state{};
        uncommitted_state.open = true;
        uncommitted_state.transaction_open = true;
        const EventSpec begin_event{.kind = EventKind::kTransactionBegin, .stream_code = StreamCode::kFortsRefdataRepl};
        table_clear_projector.on_event({}, begin_event, uncommitted_state);
        const EventSpec uncommitted_clear{
            .kind = EventKind::kClearDeleted,
            .stream_code = StreamCode::kFortsRefdataRepl,
            .table_code = TableCode::kFortsRefdataReplFutSessContents,
            .signed_value = std::numeric_limits<std::int64_t>::max(),
        };
        table_clear_projector.on_event({}, uncommitted_clear, uncommitted_state);
        require(table_clear_projector.instruments().size() == 2,
                "an uncommitted table clear must not change committed visibility");
        uncommitted_state.transaction_open = false;
        uncommitted_state.commit_count = 3;
        const EventSpec commit_event{.kind = EventKind::kTransactionCommit,
                                     .stream_code = StreamCode::kFortsRefdataRepl};
        table_clear_projector.on_transaction_commit({}, commit_event, uncommitted_state);
        require(table_clear_projector.instruments().empty(),
                "a committed MAX table clear should remove the addressed table rows");

        Plaza2PrivateStateProjector lifenum_projector;
        run_or_throw("private_state_lifenum_invalidation", &lifenum_projector);

        const auto& connector = lifenum_projector.connector_health();
        require(!connector.online, "lifenum invalidation should clear connector online state");
        require(connector.commit_count == 1, "lifenum invalidation should preserve historical commit count");

        const auto& resume = lifenum_projector.resume_markers();
        require(resume.has_lifenum && resume.last_lifenum == 11,
                "latest lifenum should be retained after invalidation");

        require(lifenum_projector.sessions().empty(), "session state should be invalidated on lifenum change");
        require(lifenum_projector.instruments().empty(), "instrument state should be invalidated on lifenum change");
        require(lifenum_projector.matching_map().empty(), "matching map should be invalidated on lifenum change");
        require(lifenum_projector.limits().empty(), "limits should be invalidated on lifenum change");
        require(lifenum_projector.positions().empty(), "positions should be invalidated on lifenum change");
        require(lifenum_projector.own_orders().empty(), "own orders should be invalidated on lifenum change");
        require(lifenum_projector.own_trades().empty(), "own trades should be invalidated on lifenum change");

        const auto lifenum_streams = lifenum_projector.stream_health();
        require(lifenum_streams.size() == 5, "all declared streams should remain addressable after invalidation");
        for (const auto& stream : lifenum_streams) {
            require(!stream.online, "stream online flags should reset after lifenum invalidation");
            require(!stream.snapshot_complete, "stream snapshot flags should reset after lifenum invalidation");
            require(stream.committed_row_count == 0, "stream row watermark should reset after lifenum invalidation");
            require(stream.last_commit_sequence == 0,
                    "stream commit watermark should reset after lifenum invalidation");
        }

        Plaza2PrivateStateProjector scoped_lifenum_projector;
        run_or_throw("private_state_scoped_lifenum", &scoped_lifenum_projector);
        EngineState scoped_state{};
        scoped_state.open = true;
        scoped_state.online = true;
        scoped_state.has_lifenum = true;
        scoped_state.last_lifenum = 8;
        const auto* preserved_status_instrument = find_instrument(scoped_lifenum_projector.instruments(), 9001);
        require(scoped_lifenum_projector.matching_map().empty() && preserved_status_instrument != nullptr &&
                    preserved_status_instrument->kind == moex::plaza2::private_state::InstrumentKind::kUnknown &&
                    preserved_status_instrument->has_current_status,
                "REFDATA LifeNum must invalidate REFDATA-owned state while preserving status state");
        require(!scoped_lifenum_projector.positions().empty() && !scoped_lifenum_projector.limits().empty(),
                "REFDATA LifeNum must preserve POS and PART state");
        require(!scoped_lifenum_projector.sessions().empty() && !scoped_lifenum_projector.instruments().empty(),
                "REFDATA LifeNum must preserve status-owned session and instrument state");
        const auto preserved_position_count = scoped_lifenum_projector.positions().size();
        const EventSpec refdata_lifenum{
            .kind = EventKind::kLifeNum, .stream_code = StreamCode::kFortsRefdataRepl, .numeric_value = 2};
        scoped_lifenum_projector.on_event({}, refdata_lifenum, scoped_state);
        require(scoped_lifenum_projector.positions().size() == preserved_position_count,
                "repeated LifeNum must not invalidate another domain");

        Plaza2PrivateStateProjector pos_lifenum_projector;
        run_or_throw("private_state_scoped_lifenum", &pos_lifenum_projector);
        const EventSpec pos_lifenum{
            .kind = EventKind::kLifeNum, .stream_code = StreamCode::kFortsPosRepl, .numeric_value = 9};
        pos_lifenum_projector.on_event({}, pos_lifenum, scoped_state);
        require(pos_lifenum_projector.positions().empty(), "POS LifeNum must invalidate POS state");
        require(!pos_lifenum_projector.limits().empty() && !pos_lifenum_projector.instruments().empty(),
                "POS LifeNum must preserve PART and REFDATA state");

        // USERORDERBOOK initial ONLINE and periodic consistency are separate
        // facts.  A periodic ClearDeleted keeps the listener current while
        // making readiness fail until a committed regular info row certifies
        // publication_state=1.
        Plaza2PrivateStateProjector userbook_projector;
        EngineState userbook_state{};
        userbook_state.open = true;
        userbook_state.streams.push_back({
            .stream_code = StreamCode::kFortsUserorderbookRepl,
            .stream_name = "FORTS_USERORDERBOOK_REPL",
        });
        userbook_projector.on_event(
            {}, EventSpec{.kind = EventKind::kOpen, .stream_code = StreamCode::kFortsUserorderbookRepl},
            userbook_state);
        commit_userbook_info(userbook_projector, userbook_state, 1, false);
        userbook_state.online = true;
        userbook_state.streams.front().online = true;
        userbook_state.streams.front().snapshot_complete = true;
        userbook_projector.on_event(
            {}, EventSpec{.kind = EventKind::kOnline, .stream_code = StreamCode::kFortsUserorderbookRepl},
            userbook_state);
        {
            const auto& health = require_userbook_health(userbook_projector);
            require(
                health.online && health.snapshot_complete && health.periodic_snapshot_consistent,
                "regular info publication_state=1 plus ONLINE should make USERORDERBOOK current and periodic-ready");
            require(health.has_publication_state && health.publication_state == 1 && health.last_trades_rev == 42 &&
                        health.last_trades_lifenum == 7 && health.last_info_moment == 1700000042,
                    "regular USERORDERBOOK info evidence should be retained");
        }

        clear_userbook_table(userbook_projector, userbook_state, TableCode::kFortsUserorderbookReplOrdersCurrentday);
        clear_userbook_table(userbook_projector, userbook_state,
                             TableCode::kFortsUserorderbookReplMultilegOrdersCurrentday);
        clear_userbook_table(userbook_projector, userbook_state, TableCode::kFortsUserorderbookReplInfoCurrentday);
        {
            const auto& health = require_userbook_health(userbook_projector);
            require(health.online && health.snapshot_complete && health.periodic_snapshot_consistent,
                    "current-day USERORDERBOOK clears must preserve regular periodic readiness");
            require(health.has_publication_state && health.publication_state == 1 && health.last_trades_rev == 42 &&
                        health.last_trades_lifenum == 7 && health.last_info_moment == 1700000042,
                    "current-day USERORDERBOOK clears must preserve regular periodic evidence");
        }

        clear_userbook_table(userbook_projector, userbook_state, TableCode::kFortsUserorderbookReplOrders);
        clear_userbook_table(userbook_projector, userbook_state, TableCode::kFortsUserorderbookReplMultilegOrders);
        {
            const auto& health = require_userbook_health(userbook_projector);
            require(health.online && health.snapshot_complete && !health.periodic_snapshot_consistent,
                    "periodic ClearDeleted must preserve ONLINE while invalidating periodic consistency");
        }

        commit_userbook_info(userbook_projector, userbook_state, 0, false);
        require(!require_userbook_health(userbook_projector).periodic_snapshot_consistent,
                "regular publication_state=0 must remain periodic-inconsistent");

        commit_userbook_info(userbook_projector, userbook_state, 1, false);
        require(require_userbook_health(userbook_projector).online &&
                    require_userbook_health(userbook_projector).periodic_snapshot_consistent,
                "committed regular publication_state=1 must restore periodic readiness without ONLINE");

        clear_userbook_table(userbook_projector, userbook_state, TableCode::kFortsUserorderbookReplOrders);
        commit_userbook_info(userbook_projector, userbook_state, 1, false);
        require(require_userbook_health(userbook_projector).periodic_snapshot_consistent,
                "committed regular info after a periodic clear must restore readiness");

        clear_userbook_table(userbook_projector, userbook_state, TableCode::kFortsUserorderbookReplOrders);
        userbook_state.transaction_open = true;
        const EventSpec uncommitted_begin{.kind = EventKind::kTransactionBegin,
                                          .stream_code = StreamCode::kFortsUserorderbookRepl};
        userbook_projector.on_event({}, uncommitted_begin, userbook_state);
        const std::vector<FieldValueSpec> uncommitted_fields = {
            {.field_code = FieldCode::kFortsUserorderbookReplInfoPublicationState,
             .kind = ValueKind::kSignedInteger,
             .signed_value = 1},
            {.field_code = FieldCode::kFortsUserorderbookReplInfoTradesRev,
             .kind = ValueKind::kSignedInteger,
             .signed_value = 43},
            {.field_code = FieldCode::kFortsUserorderbookReplInfoTradesLifenum,
             .kind = ValueKind::kSignedInteger,
             .signed_value = 7},
            {.field_code = FieldCode::kFortsUserorderbookReplInfoMoment,
             .kind = ValueKind::kSignedInteger,
             .signed_value = 1700000043},
        };
        const EventSpec uncommitted_data{.kind = EventKind::kStreamData,
                                         .stream_code = StreamCode::kFortsUserorderbookRepl,
                                         .table_code = TableCode::kFortsUserorderbookReplInfo};
        const RowSpec uncommitted_row{.stream_code = StreamCode::kFortsUserorderbookRepl,
                                      .table_code = TableCode::kFortsUserorderbookReplInfo,
                                      .field_count = static_cast<std::uint32_t>(uncommitted_fields.size())};
        userbook_projector.on_stream_row({}, uncommitted_data, uncommitted_row, uncommitted_fields, userbook_state);
        require(!require_userbook_health(userbook_projector).periodic_snapshot_consistent,
                "uncommitted regular publication_state=1 must not restore periodic readiness");
        userbook_state.transaction_open = false;
        userbook_state.commit_count += 1;
        const EventSpec uncommitted_commit{.kind = EventKind::kTransactionCommit,
                                           .stream_code = StreamCode::kFortsUserorderbookRepl};
        userbook_projector.on_transaction_commit({}, uncommitted_commit, userbook_state);
        require(require_userbook_health(userbook_projector).periodic_snapshot_consistent,
                "committed regular publication_state=1 must restore periodic readiness");

        clear_userbook_table(userbook_projector, userbook_state, TableCode::kFortsUserorderbookReplOrders);
        commit_userbook_info(userbook_projector, userbook_state, 1, true);
        {
            const auto& health = require_userbook_health(userbook_projector);
            require(health.online && health.snapshot_complete && !health.periodic_snapshot_consistent,
                    "current-day publication_state=1 must not restore an incomplete regular periodic snapshot");
            require(!health.has_publication_state && health.last_trades_rev == 0 && health.last_trades_lifenum == 0,
                    "current-day info must not replace regular USERORDERBOOK evidence");
        }

        userbook_state.streams.front().online = false;
        userbook_state.streams.front().snapshot_complete = false;
        userbook_state.online = false;
        userbook_projector.on_event({},
                                    EventSpec{.kind = EventKind::kLifeNum,
                                              .stream_code = StreamCode::kFortsUserorderbookRepl,
                                              .numeric_value = 7},
                                    userbook_state);
        userbook_projector.on_event({},
                                    EventSpec{.kind = EventKind::kLifeNum,
                                              .stream_code = StreamCode::kFortsUserorderbookRepl,
                                              .numeric_value = 8},
                                    userbook_state);
        {
            const auto& health = require_userbook_health(userbook_projector);
            require(!health.online && !health.snapshot_complete && !health.periodic_snapshot_consistent,
                    "changed USERORDERBOOK LifeNum must invalidate currentness and periodic consistency");
        }
        userbook_projector.on_event(
            {}, EventSpec{.kind = EventKind::kClose, .stream_code = StreamCode::kFortsUserorderbookRepl},
            userbook_state);
        {
            const auto& health = require_userbook_health(userbook_projector);
            require(!health.online && !health.snapshot_complete && !health.periodic_snapshot_consistent,
                    "USERORDERBOOK CLOSE must invalidate currentness and periodic consistency");
        }

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
