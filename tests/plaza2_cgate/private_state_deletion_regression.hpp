#pragma once

#include "moex/plaza2/cgate/plaza2_private_state.hpp"
#include "fixtures/server_schema.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace moex::plaza2::test::deletion {
namespace gen = generated;
namespace ps = private_state;
namespace pr = projection;
using enum gen::TableCode;

inline constexpr std::array tables{kFortsTradeReplOrdersLog,
                                   kFortsTradeReplUserDeal,
                                   kFortsTradeReplHeartbeat,
                                   kFortsTradeReplSysEvents,
                                   kFortsUserorderbookReplOrders,
                                   kFortsUserorderbookReplInfo,
                                   kFortsPosReplPosition,
                                   kFortsPosReplInfo,
                                   kFortsPartReplSysEvents,
                                   kFortsRefdataReplSession,
                                   kFortsRefdataReplFutInstruments,
                                   kFortsRefdataReplFutVcb,
                                   kFortsRefdataReplFutSessContents,
                                   kFortsRefdataReplSysMessages,
                                   kFortsSessionstateReplSessionState,
                                   kFortsInstrumentstateReplInstrumentState};

inline void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
inline gen::StreamCode stream(gen::TableCode table) {
    return static_cast<gen::StreamCode>(gen::FindTableByCode(table)->stream_id);
}
struct Row {
    std::vector<std::string> text;
    std::vector<pr::FieldValueSpec> fields;
    Row(gen::TableCode table, std::int64_t id, std::int64_t revision, std::int64_t act = 0, bool sparse = false) {
        const auto descriptors = server_schema_fields(table);
        text.reserve(descriptors.size());
        fields.reserve(descriptors.size());
        for (const auto& field : descriptors) {
            if (sparse && field.field_name != "replID" && field.field_name != "replRev" &&
                field.field_name != "replAct")
                continue;
            auto value = id;
            if (field.field_name == "replID")
                value = id + 10000; // Deliberately distinct from every natural ID.
            else if (field.field_name == "replRev")
                value = revision;
            else if (field.field_name == "replAct")
                value = act;
            else if (field.field_name == "publication_state")
                value = 1;
            else if (field.field_name == "leg_order_no" || field.field_name == "account_type")
                value = 1;
            pr::FieldValueSpec decoded{
                .field_code = field.field_code, .kind = pr::ValueKind::kSignedInteger, .signed_value = value};
            if (field.value_class == gen::ValueClass::kFixedString || field.value_class == gen::ValueClass::kDecimal) {
                text.push_back(field.value_class == gen::ValueClass::kDecimal ? "100" : "A" + std::to_string(id));
                decoded.kind =
                    field.value_class == gen::ValueClass::kDecimal ? pr::ValueKind::kDecimal : pr::ValueKind::kString;
                decoded.text_value = text.back();
            }
            fields.push_back(decoded);
        }
    }
};
struct Harness {
    ps::Plaza2PrivateStateProjector projector;
    pr::EngineState state;
    std::uint64_t rows{};
    gen::StreamCode active{};
    Harness() {
        state.open = true;
        for (const auto code :
             {gen::StreamCode::kFortsTradeRepl, gen::StreamCode::kFortsUserorderbookRepl,
              gen::StreamCode::kFortsPosRepl, gen::StreamCode::kFortsPartRepl, gen::StreamCode::kFortsRefdataRepl,
              gen::StreamCode::kFortsSessionstateRepl, gen::StreamCode::kFortsInstrumentstateRepl})
            state.streams.push_back({.stream_code = code, .online = true, .snapshot_complete = true});
    }
    void begin(gen::TableCode table, bool native = false) {
        active = stream(table);
        rows = 0;
        state.transaction_open = true;
        projector.on_event(
            {}, {.kind = pr::EventKind::kTransactionBegin, .stream_code = active, .numeric_value = native ? 1u : 0u},
            state);
    }
    void row(gen::TableCode table, std::int64_t id, std::int64_t revision, std::int64_t act = 0, bool sparse = false,
             std::int64_t repl_id = 0) {
        Row values(table, id, revision, act, sparse);
        if (repl_id)
            values.fields[0].signed_value = repl_id;
        projector.on_stream_row({},
                                {.kind = pr::EventKind::kStreamData,
                                 .stream_code = stream(table),
                                 .table_code = table,
                                 .signed_value = revision},
                                {}, values.fields, state);
        ++rows;
    }
    void commit() {
        state.transaction_open = false;
        ++state.commit_count;
        for (auto& value : state.streams)
            if (value.stream_code == active)
                value.committed_row_count += rows;
        const pr::EventSpec event{.kind = pr::EventKind::kTransactionCommit, .stream_code = active};
        projector.on_event({}, event, state);
        projector.on_transaction_commit({}, event, state);
    }
    const ps::StreamHealthSnapshot& health(gen::TableCode table) const {
        const auto values = projector.stream_health();
        const auto found = std::find_if(values.begin(), values.end(),
                                        [&](const auto& value) { return value.stream_code == stream(table); });
        require(found != values.end(), "missing stream health");
        return *found;
    }
    bool has(gen::TableCode table, std::int64_t id) const {
        const auto any = [id](const auto& values, const auto& matches) {
            return std::any_of(values.begin(), values.end(), [&](const auto& value) { return matches(value, id); });
        };
        switch (table) {
        case kFortsTradeReplOrdersLog:
        case kFortsTradeReplMultilegOrdersLog:
        case kFortsUserorderbookReplOrders:
        case kFortsUserorderbookReplMultilegOrders:
        case kFortsUserorderbookReplOrdersCurrentday:
        case kFortsUserorderbookReplMultilegOrdersCurrentday:
            return any(projector.own_orders(),
                       [](const auto& value, auto key) { return value.public_order_id == key; });
        case kFortsTradeReplUserDeal:
        case kFortsTradeReplUserMultilegDeal:
            return any(projector.own_trades(), [](const auto& value, auto key) { return value.id_deal == key; });
        case kFortsPosReplPosition:
            return any(projector.positions(), [](const auto& value, auto key) { return value.isin_id == key; });
        case kFortsPartReplPart:
            return any(projector.limits(), [](const auto& value, auto key) { return value.repl_id == key + 10000; });
        case kFortsRefdataReplSession:
            return any(projector.sessions(), [](const auto& value, auto key) { return value.sess_id == key; });
        case kFortsRefdataReplFutInstruments:
        case kFortsRefdataReplFutSessContents:
        case kFortsRefdataReplOptSessContents:
            return any(projector.instruments(), [](const auto& value, auto key) { return value.isin_id == key; });
        case kFortsSessionstateReplSessionState:
            return any(projector.sessions(),
                       [](const auto& value, auto key) { return value.sess_id == key && value.has_current_status; });
        case kFortsInstrumentstateReplInstrumentState:
            return any(projector.instruments(),
                       [](const auto& value, auto key) { return value.isin_id == key && value.has_current_status; });
        case kFortsRefdataReplFutVcb:
            return any(projector.future_vcb(),
                       [](const auto& value, auto key) { return value.repl_id == key + 10000; });
        case kFortsRefdataReplMultilegDict:
            return any(projector.instruments(),
                       [](const auto& value, auto key) { return value.isin_id == key && !value.legs.empty(); });
        case kFortsRefdataReplInstr2matchingMap:
            return any(projector.matching_map(),
                       [](const auto& value, auto key) { return value.base_contract_id == key; });
        case kFortsRefdataReplSysMessages:
            return any(projector.system_messages(),
                       [](const auto& value, auto key) { return value.repl_id == key + 10000; });
        case kFortsTradeReplHeartbeat:
            return health(table).last_server_time == id;
        case kFortsTradeReplSysEvents:
        case kFortsPartReplSysEvents:
            return health(table).last_event_id == id;
        case kFortsPosReplInfo:
        case kFortsUserorderbookReplInfo:
            return health(table).last_trades_rev == id;
        case kFortsUserorderbookReplInfoCurrentday:
            // Deliberately unconsumed: neither active nor deleted current-day
            // info may certify the periodic USERORDERBOOK snapshot.
            return false;
        default:
            throw std::runtime_error("uncovered deletion table");
        }
    }
};
inline bool marker(gen::TableCode table) {
    return table == kFortsTradeReplHeartbeat || table == kFortsTradeReplSysEvents || table == kFortsPartReplSysEvents ||
           table == kFortsPosReplInfo || table == kFortsUserorderbookReplInfo ||
           table == kFortsUserorderbookReplInfoCurrentday;
}
inline void one_table(gen::TableCode table, bool sparse, bool native) {
    Harness h;
    h.begin(table, native);
    h.row(table, 10, 1);
    h.row(table, 20, 2);
    h.commit();
    const bool ignored = table == kFortsUserorderbookReplInfoCurrentday;
    require(h.has(table, 20) || ignored, "active row did not reach its consumed view");
    h.begin(table, native);
    h.row(table, 10, 3, 256, sparse); // Any nonzero i8 value is a tombstone.
    h.commit();
    require(!h.has(table, 10), "deleted row remained live");
    require(h.has(table, 20) || ignored, "deletion erased an unrelated record/marker");
    require(h.health(table).committed_row_count == 3 && h.health(table).last_commit_sequence == 2,
            "row deletion reset instead of advancing commit/row watermarks");
    h.begin(table, native);
    h.row(table, 20, 4, -1, sparse);
    h.commit();
    require(!h.has(table, 20), "deleted latest row remained live");
    h.begin(table, native);
    h.row(table, 10, 5, 1, sparse); // Unknown/repeated tombstone must not create a row.
    h.commit();
    require(!h.has(table, 10) && !h.has(table, 20), "repeated tombstone manufactured a live record");
}
inline void rollback_and_sources() {
    Harness h;
    h.begin(kFortsTradeReplOrdersLog);
    h.row(kFortsTradeReplOrdersLog, 10, 1);
    h.row(kFortsUserorderbookReplOrders, 10, 1);
    h.row(kFortsUserorderbookReplOrders, 20, 2);
    h.commit();
    (void)h.projector.take_row_changes();
    h.begin(kFortsUserorderbookReplOrders);
    h.row(kFortsUserorderbookReplOrders, 10, 3, 1, true);
    require(h.projector.own_orders().size() == 3, "uncommitted deletion escaped its transaction");
    // Beginning again discards the uncommitted deletion and its replica index.
    h.begin(kFortsUserorderbookReplOrders);
    h.commit();
    require(h.projector.own_orders().size() == 3, "rollback lost a source row/index");
    h.begin(kFortsUserorderbookReplOrders, true);
    h.row(kFortsUserorderbookReplOrders, 10, 4, 1, true);
    h.commit();
    require(h.projector.own_orders().size() == 2 &&
                std::any_of(h.projector.own_orders().begin(), h.projector.own_orders().end(),
                            [](const auto& value) {
                                return value.public_order_id == 10 && value.from_trade_repl && !value.from_user_book;
                            }) &&
                std::none_of(h.projector.own_orders().begin(), h.projector.own_orders().end(),
                             [](const auto& value) { return value.public_order_id == 10 && value.from_user_book; }),
            "sparse USERORDERBOOK deletion removed the independent same-ID TRADE source");
    const auto changes = h.projector.take_row_changes();
    require(!changes.resync_required && changes.orders.empty(),
            "USERORDERBOOK deletion unnecessarily invalidated the TRADE delta consumer");
    auto cloned = h.projector.clone();
    const auto old = std::move(h.projector);
    h.projector = std::move(cloned);
    h.begin(kFortsTradeReplOrdersLog, true);
    h.row(kFortsTradeReplOrdersLog, 10, 5, 1, true);
    h.commit();
    require(h.projector.own_orders().size() == 1 && old.own_orders().size() == 2,
            "cloned deletion index references or modifies its original projector");
}
inline void independent_userbook_tables() {
    Harness h;
    h.begin(kFortsUserorderbookReplOrders, true);
    h.row(kFortsUserorderbookReplOrders, 10, 1);
    h.row(kFortsUserorderbookReplOrdersCurrentday, 10, 1);
    h.commit();
    require(h.projector.own_orders().size() == 1 && h.projector.own_orders()[0].from_user_book,
            "unconsumed current-day row replaced the regular USERORDERBOOK record");
    h.begin(kFortsUserorderbookReplOrdersCurrentday, true);
    h.row(kFortsUserorderbookReplOrdersCurrentday, 10, 2, 1, true);
    h.commit();
    require(h.projector.own_orders().size() == 1 && h.projector.own_orders()[0].from_user_book &&
                !h.projector.own_orders()[0].from_current_day,
            "current-day deletion removed an independent regular USERORDERBOOK record");
    h.begin(kFortsUserorderbookReplOrdersCurrentday, true);
    h.row(kFortsUserorderbookReplOrdersCurrentday, 10, 3);
    h.commit();
    h.begin(kFortsUserorderbookReplOrders, true);
    h.row(kFortsUserorderbookReplOrders, 10, 4, 1, true);
    h.commit();
    require(h.projector.own_orders().empty(), "unconsumed current-day row survived regular USERORDERBOOK deletion");
}
inline void replica_identity_update() {
    Harness h;
    h.begin(kFortsPosReplPosition, true);
    h.row(kFortsPosReplPosition, 10, 1);
    h.commit();
    h.begin(kFortsPosReplPosition, true);
    h.row(kFortsPosReplPosition, 20, 2, 0, false, 10010);
    h.commit();
    require(!h.has(kFortsPosReplPosition, 10) && h.has(kFortsPosReplPosition, 20),
            "changed natural fields duplicated a single physical replication row");
    h.begin(kFortsPosReplPosition, true);
    h.row(kFortsPosReplPosition, 10, 3, 1, true);
    h.commit();
    require(h.projector.positions().empty(), "sparse tombstone did not follow updated replication identity");
}
inline void epoch_and_reinsert() {
    for (const auto table : {kFortsTradeReplOrdersLog, kFortsTradeReplUserDeal, kFortsPosReplPosition}) {
        for (const int invalidation : {0, 1, 2}) {
            Harness h;
            h.projector.on_event(
                {}, {.kind = pr::EventKind::kLifeNum, .stream_code = stream(table), .numeric_value = 1}, h.state);
            h.begin(table, true);
            h.row(table, 10, 1);
            h.row(table, 20, 3);
            h.commit();
            if (invalidation == 0) {
                h.begin(table, true);
                h.projector.on_event({},
                                     {.kind = pr::EventKind::kClearDeleted,
                                      .stream_code = stream(table),
                                      .table_code = table,
                                      .signed_value = 2},
                                     h.state);
                h.commit();
                require(!h.has(table, 10) && h.has(table, 20), "finite clear damaged retained replication rows");
            } else if (invalidation == 1)
                h.projector.on_event(
                    {}, {.kind = pr::EventKind::kLifeNum, .stream_code = stream(table), .numeric_value = 2}, h.state);
            else
                h.projector.reset_stream_snapshot(stream(table));
            h.begin(table, true);
            h.row(table, 10, 4, 0, false, 20010);
            h.commit();
            h.begin(table, true);
            h.row(table, 10, 5, 1, true); // Obsolete physical ID must not erase its replacement.
            h.commit();
            require(h.has(table, 10), "epoch/clear retained an obsolete physical row mapping");
            h.begin(table, true);
            h.row(table, 10, 6, 1, true, 20010);
            h.commit();
            require(!h.has(table, 10), "replacement physical ID was missing after epoch/clear");
        }
    }
}
inline void independent_marker_time() {
    Harness h;
    h.begin(kFortsTradeReplSysEvents, true);
    h.row(kFortsTradeReplSysEvents, 10, 1);
    h.row(kFortsTradeReplHeartbeat, 20, 2);
    h.commit();
    h.begin(kFortsTradeReplSysEvents, true);
    h.row(kFortsTradeReplSysEvents, 10, 3, 1, true);
    h.commit();
    require(h.health(kFortsTradeReplHeartbeat).last_server_time == 20,
            "deleted system event cleared an independent newer heartbeat time");
}
inline void independent_status_sources() {
    Harness h;
    h.begin(kFortsRefdataReplSession, true);
    h.row(kFortsRefdataReplSession, 10, 1);
    h.row(kFortsSessionstateReplSessionState, 10, 1);
    h.row(kFortsRefdataReplFutInstruments, 10, 1);
    h.row(kFortsInstrumentstateReplInstrumentState, 10, 1);
    h.commit();
    h.begin(kFortsRefdataReplSession, true);
    h.row(kFortsRefdataReplSession, 10, 2, 1, true);
    h.row(kFortsRefdataReplFutInstruments, 10, 2, 1, true);
    h.commit();
    require(h.has(kFortsSessionstateReplSessionState, 10) && h.has(kFortsInstrumentstateReplInstrumentState, 10) &&
                h.projector.sessions()[0].begin == 0 &&
                h.projector.instruments()[0].kind == ps::InstrumentKind::kUnknown,
            "REFDATA deletion erased independent status or retained deleted definition fields");
    h.begin(kFortsSessionstateReplSessionState, true);
    h.row(kFortsSessionstateReplSessionState, 10, 3, 1, true);
    h.row(kFortsInstrumentstateReplInstrumentState, 10, 3, 1, true);
    h.commit();
    require(h.projector.sessions().empty() && h.projector.instruments().empty(),
            "last-source deletion retained status-only metadata");

    h.begin(kFortsRefdataReplSession, true);
    h.row(kFortsRefdataReplSession, 20, 4);
    h.row(kFortsSessionstateReplSessionState, 20, 4);
    h.row(kFortsRefdataReplFutInstruments, 20, 4);
    h.row(kFortsInstrumentstateReplInstrumentState, 20, 4);
    h.commit();
    h.begin(kFortsSessionstateReplSessionState, true);
    h.row(kFortsSessionstateReplSessionState, 20, 5, 1, true);
    h.row(kFortsInstrumentstateReplInstrumentState, 20, 5, 1, true);
    h.commit();
    require(h.has(kFortsRefdataReplSession, 20) && h.has(kFortsRefdataReplFutInstruments, 20) &&
                !h.has(kFortsSessionstateReplSessionState, 20) && !h.has(kFortsInstrumentstateReplInstrumentState, 20),
            "status deletion erased an independent reference definition");
}
inline void native_batch_deletion() {
    Harness h;
    h.begin(kFortsTradeReplOrdersLog, true);
    for (const auto id : {10, 20, 30})
        h.row(kFortsTradeReplOrdersLog, id, id);
    h.commit();
    const auto first = h.projector.own_orders().front().public_order_id;
    const auto last = h.projector.own_orders().back().public_order_id;
    h.begin(kFortsTradeReplOrdersLog, true);
    h.row(kFortsTradeReplOrdersLog, first, 40, 1, true);
    h.row(kFortsTradeReplOrdersLog, last, 41, 1, true);
    h.commit();
    require(h.projector.own_orders().size() == 1 && !h.has(kFortsTradeReplOrdersLog, first) &&
                !h.has(kFortsTradeReplOrdersLog, last),
            "native batch deletion corrupted its compact snapshot index");
    const auto retained = h.projector.own_orders()[0].public_order_id;
    h.begin(kFortsTradeReplOrdersLog, true);
    h.row(kFortsTradeReplOrdersLog, retained, 42);
    h.commit();
    require(h.projector.own_orders().size() == 1 && h.has(kFortsTradeReplOrdersLog, retained),
            "native batch deletion left a stale view slot for its survivor");
}
inline void run() {
    unsigned failures{};
    for (const auto table : tables) {
        for (const bool sparse : {false, true})
            for (const bool native : {false, true})
                try {
                    one_table(table, sparse, native);
                } catch (const std::exception& error) {
                    ++failures;
                    const auto* descriptor = gen::FindTableByCode(table);
                    std::cerr << descriptor->stream_name << '.' << descriptor->table_name
                              << (sparse ? " sparse" : " full") << (native ? " native" : " staged") << ": "
                              << error.what() << '\n';
                }
    }
    try {
        rollback_and_sources();
        independent_userbook_tables();
        replica_identity_update();
        epoch_and_reinsert();
        independent_marker_time();
        independent_status_sources();
        native_batch_deletion();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "rollback/independent-source/clone: " << error.what() << '\n';
    }
    require(failures == 0, "replication deletion regression failed");
}
} // namespace moex::plaza2::test::deletion
