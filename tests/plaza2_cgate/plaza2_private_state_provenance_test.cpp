#include "moex/plaza2/cgate/plaza2_private_state.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using moex::plaza2::fake::EngineState;
using moex::plaza2::fake::EventKind;
using moex::plaza2::fake::EventSpec;
using moex::plaza2::fake::FieldValueSpec;
using moex::plaza2::fake::Plaza2FakeEngine;
using moex::plaza2::fake::RowSpec;
using moex::plaza2::fake::ScenarioDataView;
using moex::plaza2::fake::ScenarioSpec;
using moex::plaza2::fake::ValueKind;
using moex::plaza2::generated::FieldCode;
using moex::plaza2::generated::StreamCode;
using moex::plaza2::generated::TableCode;
using moex::plaza2::private_state::InstrumentSnapshot;
using moex::plaza2::private_state::Plaza2PrivateStateProjector;
using moex::plaza2::private_state::SourceRowProvenance;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

FieldValueSpec signed_field(FieldCode field_code, std::int64_t value) {
    return {
        .field_code = field_code,
        .kind = ValueKind::kSignedInteger,
        .signed_value = value,
    };
}

FieldValueSpec text_field(FieldCode field_code, std::string_view value) {
    return {
        .field_code = field_code,
        .kind = ValueKind::kString,
        .text_value = value,
    };
}

const InstrumentSnapshot* find_instrument(std::span<const InstrumentSnapshot> instruments, std::int32_t isin_id) {
    for (const auto& instrument : instruments) {
        if (instrument.isin_id == isin_id) {
            return &instrument;
        }
    }
    return nullptr;
}

const SourceRowProvenance& require_provenance(const std::optional<SourceRowProvenance>& provenance,
                                              TableCode table_code, std::int64_t revision, std::uint64_t lifenum,
                                              const char* message) {
    require(provenance.has_value(), message);
    require(provenance->present, "a committed source row must report present=true");
    require(provenance->stream_code == StreamCode::kFortsRefdataRepl,
            "target provenance must identify FORTS_REFDATA_REPL");
    require(provenance->table_code == table_code, "target provenance table code mismatch");
    require(provenance->repl_rev == revision, "target provenance revision mismatch");
    require(provenance->lifenum == lifenum, "target provenance LifeNum mismatch");
    return *provenance;
}

struct InitialScenario {
    std::vector<StreamCode> streams = {
        StreamCode::kFortsRefdataRepl,
        StreamCode::kFortsInstrumentstateRepl,
    };
    std::vector<EventSpec> events;
    std::vector<RowSpec> rows;
    std::vector<FieldValueSpec> fields;
    ScenarioSpec scenario{
        .scenario_id = "private_state_refdata_provenance",
        .description = "typed REFDATA row provenance",
        .metadata_version = 1,
        .deterministic_seed = 20260902,
    };

    InitialScenario() {
        add_simple(EventKind::kOpen);
        add_simple(EventKind::kLifeNum, StreamCode::kFortsRefdataRepl, 7);
        add_simple(EventKind::kTransactionBegin, StreamCode::kFortsRefdataRepl);
        add_row(
            StreamCode::kFortsRefdataRepl, TableCode::kFortsRefdataReplFutInstruments, 10,
            {
                signed_field(FieldCode::kFortsRefdataReplFutInstrumentsIsinId, 1001),
                text_field(FieldCode::kFortsRefdataReplFutInstrumentsIsin, "TARGET-OLD"),
                text_field(FieldCode::kFortsRefdataReplFutInstrumentsShortIsin, "TGT6"),
                text_field(FieldCode::kFortsRefdataReplFutInstrumentsName, "Target old"),
                text_field(FieldCode::kFortsRefdataReplFutInstrumentsMinStep, "250"),
                signed_field(FieldCode::kFortsRefdataReplFutInstrumentsTradeModeId, 4),
            });
        add_row(
            StreamCode::kFortsRefdataRepl, TableCode::kFortsRefdataReplFutSessContents, 20,
            {
                signed_field(FieldCode::kFortsRefdataReplFutSessContentsIsinId, 1001),
                signed_field(FieldCode::kFortsRefdataReplFutSessContentsSessId, 321),
                signed_field(FieldCode::kFortsRefdataReplFutSessContentsReplAct, 0),
                signed_field(FieldCode::kFortsRefdataReplFutSessContentsState, 1),
                text_field(FieldCode::kFortsRefdataReplFutSessContentsIsin, "TARGET-OLD"),
                text_field(FieldCode::kFortsRefdataReplFutSessContentsShortIsin, "TGT6"),
                text_field(FieldCode::kFortsRefdataReplFutSessContentsName, "Target old"),
                text_field(FieldCode::kFortsRefdataReplFutSessContentsMinStep, "250"),
                signed_field(FieldCode::kFortsRefdataReplFutSessContentsTradeModeId, 4),
            });
        add_row(
            StreamCode::kFortsRefdataRepl, TableCode::kFortsRefdataReplSession, 30,
            {
                signed_field(FieldCode::kFortsRefdataReplSessionSessId, 321),
                signed_field(FieldCode::kFortsRefdataReplSessionBegin, 1700000000),
                signed_field(FieldCode::kFortsRefdataReplSessionEnd, 1700003600),
                signed_field(FieldCode::kFortsRefdataReplSessionState, 1),
            });
        add_row(
            StreamCode::kFortsInstrumentstateRepl, TableCode::kFortsInstrumentstateReplInstrumentState, 40,
            {
                signed_field(FieldCode::kFortsInstrumentstateReplInstrumentStateIsinId, 1001),
                signed_field(FieldCode::kFortsInstrumentstateReplInstrumentStatePublicState, 1),
            });
        add_simple(EventKind::kTransactionCommit, StreamCode::kFortsRefdataRepl);
    }

    ScenarioDataView view() const {
        return {
            .scenario = scenario,
            .streams = streams,
            .events = events,
            .rows = rows,
            .fields = fields,
            .invariants = {},
        };
    }

  private:
    void add_simple(EventKind kind, StreamCode stream_code = moex::plaza2::fake::kNoStreamCode,
                    std::uint64_t numeric_value = 0) {
        events.push_back({
            .kind = kind,
            .stream_code = stream_code,
            .numeric_value = numeric_value,
        });
    }

    void add_row(StreamCode stream_code, TableCode table_code, std::int64_t revision,
                 std::initializer_list<FieldValueSpec> row_fields) {
        const auto first_field_index = fields.size();
        fields.insert(fields.end(), row_fields.begin(), row_fields.end());
        rows.push_back({
            .stream_code = stream_code,
            .table_code = table_code,
            .first_field_index = static_cast<std::uint32_t>(first_field_index),
            .field_count = static_cast<std::uint32_t>(row_fields.size()),
        });
        events.push_back({
            .kind = EventKind::kStreamData,
            .stream_code = stream_code,
            .table_code = table_code,
            .first_row_index = static_cast<std::uint32_t>(rows.size() - 1),
            .row_count = 1,
            .signed_value = revision,
        });
    }
};

std::size_t stream_index(const EngineState& state, StreamCode stream_code) {
    for (std::size_t index = 0; index < state.streams.size(); ++index) {
        if (state.streams[index].stream_code == stream_code) {
            return index;
        }
    }
    return state.streams.size();
}

void begin_transaction(Plaza2PrivateStateProjector& projector, EngineState& state, StreamCode stream_code) {
    state.transaction_open = true;
    projector.on_event({}, EventSpec{.kind = EventKind::kTransactionBegin, .stream_code = stream_code}, state);
}

void stage_row(Plaza2PrivateStateProjector& projector, EngineState& state, StreamCode stream_code,
               TableCode table_code, std::int64_t revision, std::initializer_list<FieldValueSpec> row_fields) {
    const std::vector<FieldValueSpec> fields(row_fields);
    const EventSpec event{
        .kind = EventKind::kStreamData,
        .stream_code = stream_code,
        .table_code = table_code,
        .signed_value = revision,
    };
    const RowSpec row{
        .stream_code = stream_code,
        .table_code = table_code,
        .field_count = static_cast<std::uint32_t>(fields.size()),
    };
    projector.on_stream_row({}, event, row, fields, state);
}

void commit_transaction(Plaza2PrivateStateProjector& projector, EngineState& state, StreamCode stream_code,
                        std::uint64_t row_count) {
    state.transaction_open = false;
    state.commit_count += 1;
    const auto index = stream_index(state, stream_code);
    require(index < state.streams.size(), "manual transaction stream must be declared");
    state.streams[index].committed_row_count += row_count;
    const EventSpec event{.kind = EventKind::kTransactionCommit, .stream_code = stream_code};
    projector.on_event({}, event, state);
    projector.on_transaction_commit({}, event, state);
}

void clear_table(Plaza2PrivateStateProjector& projector, EngineState& state, StreamCode stream_code,
                 TableCode table_code, std::int64_t clear_revision) {
    projector.on_event({}, EventSpec{
                              .kind = EventKind::kClearDeleted,
                              .stream_code = stream_code,
                              .table_code = table_code,
                              .signed_value = clear_revision,
                          },
                       state);
}

void set_lifenum(Plaza2PrivateStateProjector& projector, EngineState& state, StreamCode stream_code,
                 std::uint64_t lifenum) {
    state.has_lifenum = true;
    state.last_lifenum = lifenum;
    projector.on_event({}, EventSpec{
                              .kind = EventKind::kLifeNum,
                              .stream_code = stream_code,
                              .numeric_value = lifenum,
                          },
                       state);
}

} // namespace

int main() {
    try {
        InitialScenario scenario;
        Plaza2PrivateStateProjector projector;
        Plaza2FakeEngine engine;
        const auto result = engine.run(scenario.view(), &projector);
        if (result.error) {
            throw std::runtime_error("initial provenance replay failed: " + result.error.message);
        }

        require(projector.refdata_lifenum().has_value() && projector.refdata_lifenum().value() == 7,
                "initial REFDATA LifeNum should be exposed");
        const auto initial_instruments = projector.instruments();
        const auto* initial_target = find_instrument(initial_instruments, 1001);
        require(initial_target != nullptr && initial_target->isin == "TARGET-OLD",
                "initial target instrument should be committed");

        const auto fut_instruments = require_provenance(
            projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutInstruments, 1001),
            TableCode::kFortsRefdataReplFutInstruments, 10, 7, "fut_instruments provenance should be present");
        const auto fut_session_contents = require_provenance(
            projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutSessContents, 1001),
            TableCode::kFortsRefdataReplFutSessContents, 20, 7,
            "fut_sess_contents provenance should be present");
        require(fut_instruments.repl_rev != fut_session_contents.repl_rev,
                "merged CRU6-like instrument tables must retain independent revisions");
        static_cast<void>(require_provenance(
            projector.session_source_provenance(TableCode::kFortsRefdataReplSession, 321),
            TableCode::kFortsRefdataReplSession, 30, 7, "session provenance should be present"));
        require(!projector.instrument_source_provenance(TableCode::kFortsRefdataReplSession, 1001).has_value(),
                "typed instrument query must reject the session table");

        EngineState state = result.state;
        const auto old_instruments_provenance = projector.instrument_source_provenance(
            TableCode::kFortsRefdataReplFutInstruments, 1001);
        const auto old_session_contents_provenance = projector.instrument_source_provenance(
            TableCode::kFortsRefdataReplFutSessContents, 1001);

        // A status-only update is a separate stream/table and must not alter
        // either REFDATA source revision.
        begin_transaction(projector, state, StreamCode::kFortsInstrumentstateRepl);
        stage_row(projector, state, StreamCode::kFortsInstrumentstateRepl,
                  TableCode::kFortsInstrumentstateReplInstrumentState, 41,
                  {
                      signed_field(FieldCode::kFortsInstrumentstateReplInstrumentStateIsinId, 1001),
                      signed_field(FieldCode::kFortsInstrumentstateReplInstrumentStatePublicState, 2),
                  });
        commit_transaction(projector, state, StreamCode::kFortsInstrumentstateRepl, 1);
        require(projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutInstruments, 1001) ==
                    old_instruments_provenance,
                "INSTRUMENTSTATE-only update must not change fut_instruments provenance");
        require(projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutSessContents, 1001) ==
                    old_session_contents_provenance,
                "INSTRUMENTSTATE-only update must not change fut_sess_contents provenance");

        // Update values and source revision together, but expose both only at
        // the transaction commit boundary.
        begin_transaction(projector, state, StreamCode::kFortsRefdataRepl);
        stage_row(projector, state, StreamCode::kFortsRefdataRepl,
                  TableCode::kFortsRefdataReplFutInstruments, 50,
                  {
                      signed_field(FieldCode::kFortsRefdataReplFutInstrumentsIsinId, 1001),
                      text_field(FieldCode::kFortsRefdataReplFutInstrumentsIsin, "TARGET-NEW"),
                      text_field(FieldCode::kFortsRefdataReplFutInstrumentsShortIsin, "TGT7"),
                      text_field(FieldCode::kFortsRefdataReplFutInstrumentsName, "Target new"),
                      text_field(FieldCode::kFortsRefdataReplFutInstrumentsMinStep, "500"),
                      signed_field(FieldCode::kFortsRefdataReplFutInstrumentsTradeModeId, 8),
                  });
        const auto* staged_target = find_instrument(projector.instruments(), 1001);
        require(staged_target != nullptr && staged_target->isin == "TARGET-OLD",
                "target values must remain old while an update is staged");
        require_provenance(projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutInstruments, 1001),
                           TableCode::kFortsRefdataReplFutInstruments, 10, 7,
                           "target provenance must remain old while an update is staged");
        commit_transaction(projector, state, StreamCode::kFortsRefdataRepl, 1);
        const auto* updated_target = find_instrument(projector.instruments(), 1001);
        require(updated_target != nullptr && updated_target->isin == "TARGET-NEW" && updated_target->min_step == "500",
                "target values should update at commit");
        require_provenance(projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutInstruments, 1001),
                           TableCode::kFortsRefdataReplFutInstruments, 50, 7,
                           "target provenance should update atomically with target values");
        require_provenance(projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutSessContents, 1001),
                           TableCode::kFortsRefdataReplFutSessContents, 20, 7,
                           "independent fut_sess_contents provenance must survive fut_instruments update");

        // CLEARDELETED on another table and a non-deleting threshold for the
        // same table must leave the target receipt unchanged.
        clear_table(projector, state, StreamCode::kFortsRefdataRepl,
                    TableCode::kFortsRefdataReplInstr2matchingMap, std::numeric_limits<std::int64_t>::max());
        require_provenance(projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutInstruments, 1001),
                           TableCode::kFortsRefdataReplFutInstruments, 50, 7,
                           "unrelated-table CLEARDELETED must not alter target provenance");
        clear_table(projector, state, StreamCode::kFortsRefdataRepl,
                    TableCode::kFortsRefdataReplFutInstruments, 50);
        require_provenance(projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutInstruments, 1001),
                           TableCode::kFortsRefdataReplFutInstruments, 50, 7,
                           "clear threshold at the row revision must preserve target provenance");

        // A deleting threshold removes the addressed source row even though
        // the merged instrument remains supplied by other tables.
        clear_table(projector, state, StreamCode::kFortsRefdataRepl,
                    TableCode::kFortsRefdataReplFutInstruments, 51);
        require(!projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutInstruments, 1001).has_value(),
                "deleting fut_instruments CLEARDELETED must remove its provenance");
        require(find_instrument(projector.instruments(), 1001) != nullptr,
                "merged instrument may survive while one source table is cleared");
        require_provenance(projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutSessContents, 1001),
                           TableCode::kFortsRefdataReplFutSessContents, 20, 7,
                           "clearing fut_instruments must preserve independent fut_sess_contents provenance");

        clear_table(projector, state, StreamCode::kFortsRefdataRepl,
                    TableCode::kFortsRefdataReplFutSessContents, std::numeric_limits<std::int64_t>::max());
        require(!projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutSessContents, 1001).has_value(),
                "MAX fut_sess_contents CLEARDELETED must remove its provenance");
        clear_table(projector, state, StreamCode::kFortsRefdataRepl, TableCode::kFortsRefdataReplSession,
                    std::numeric_limits<std::int64_t>::max());
        require(!projector.session_source_provenance(TableCode::kFortsRefdataReplSession, 321).has_value(),
                "MAX session CLEARDELETED must remove session provenance");

        // Rebuild all target rows in the old epoch, then prove that a changed
        // REFDATA LifeNum invalidates every old source receipt.
        begin_transaction(projector, state, StreamCode::kFortsRefdataRepl);
        stage_row(projector, state, StreamCode::kFortsRefdataRepl,
                  TableCode::kFortsRefdataReplFutInstruments, 60,
                  {
                      signed_field(FieldCode::kFortsRefdataReplFutInstrumentsIsinId, 1001),
                      text_field(FieldCode::kFortsRefdataReplFutInstrumentsIsin, "TARGET-EPOCH-7"),
                      text_field(FieldCode::kFortsRefdataReplFutInstrumentsMinStep, "250"),
                      signed_field(FieldCode::kFortsRefdataReplFutInstrumentsTradeModeId, 4),
                  });
        stage_row(projector, state, StreamCode::kFortsRefdataRepl,
                  TableCode::kFortsRefdataReplFutSessContents, 70,
                  {
                      signed_field(FieldCode::kFortsRefdataReplFutSessContentsIsinId, 1001),
                      signed_field(FieldCode::kFortsRefdataReplFutSessContentsSessId, 321),
                      signed_field(FieldCode::kFortsRefdataReplFutSessContentsReplAct, 0),
                      signed_field(FieldCode::kFortsRefdataReplFutSessContentsState, 1),
                  });
        stage_row(projector, state, StreamCode::kFortsRefdataRepl, TableCode::kFortsRefdataReplSession, 80,
                  {
                      signed_field(FieldCode::kFortsRefdataReplSessionSessId, 321),
                      signed_field(FieldCode::kFortsRefdataReplSessionState, 1),
                  });
        commit_transaction(projector, state, StreamCode::kFortsRefdataRepl, 3);
        require_provenance(projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutInstruments, 1001),
                           TableCode::kFortsRefdataReplFutInstruments, 60, 7,
                           "rebuilt epoch-7 fut_instruments provenance should be present");
        set_lifenum(projector, state, StreamCode::kFortsRefdataRepl, 8);
        require(projector.refdata_lifenum().has_value() && projector.refdata_lifenum().value() == 8,
                "changed REFDATA LifeNum should be exposed immediately");
        require(!projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutInstruments, 1001).has_value() &&
                    !projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutSessContents, 1001).has_value() &&
                    !projector.session_source_provenance(TableCode::kFortsRefdataReplSession, 321).has_value(),
                "changed REFDATA LifeNum must invalidate all old target provenance");

        // Rebuild in the new epoch and prove clone/reset semantics.
        begin_transaction(projector, state, StreamCode::kFortsRefdataRepl);
        stage_row(projector, state, StreamCode::kFortsRefdataRepl,
                  TableCode::kFortsRefdataReplFutInstruments, 90,
                  {
                      signed_field(FieldCode::kFortsRefdataReplFutInstrumentsIsinId, 1001),
                      text_field(FieldCode::kFortsRefdataReplFutInstrumentsIsin, "TARGET-EPOCH-8"),
                      text_field(FieldCode::kFortsRefdataReplFutInstrumentsMinStep, "250"),
                      signed_field(FieldCode::kFortsRefdataReplFutInstrumentsTradeModeId, 4),
                  });
        stage_row(projector, state, StreamCode::kFortsRefdataRepl,
                  TableCode::kFortsRefdataReplFutSessContents, 91,
                  {
                      signed_field(FieldCode::kFortsRefdataReplFutSessContentsIsinId, 1001),
                      signed_field(FieldCode::kFortsRefdataReplFutSessContentsSessId, 321),
                      signed_field(FieldCode::kFortsRefdataReplFutSessContentsReplAct, 0),
                      signed_field(FieldCode::kFortsRefdataReplFutSessContentsState, 1),
                  });
        stage_row(projector, state, StreamCode::kFortsRefdataRepl, TableCode::kFortsRefdataReplSession, 92,
                  {
                      signed_field(FieldCode::kFortsRefdataReplSessionSessId, 321),
                      signed_field(FieldCode::kFortsRefdataReplSessionState, 1),
                  });
        commit_transaction(projector, state, StreamCode::kFortsRefdataRepl, 3);

        const auto clone = projector.clone();
        require_provenance(clone.instrument_source_provenance(TableCode::kFortsRefdataReplFutInstruments, 1001),
                           TableCode::kFortsRefdataReplFutInstruments, 90, 8,
                           "clone must preserve committed target provenance");
        require_provenance(clone.instrument_source_provenance(TableCode::kFortsRefdataReplFutSessContents, 1001),
                           TableCode::kFortsRefdataReplFutSessContents, 91, 8,
                           "clone must preserve independent session-contents provenance");
        require_provenance(clone.session_source_provenance(TableCode::kFortsRefdataReplSession, 321),
                           TableCode::kFortsRefdataReplSession, 92, 8,
                           "clone must preserve session provenance");
        projector.reset();
        require(!projector.refdata_lifenum().has_value() && projector.instruments().empty() &&
                    !projector.instrument_source_provenance(TableCode::kFortsRefdataReplFutInstruments, 1001).has_value(),
                "reset must clear LifeNum, target state, and provenance");
        require(clone.refdata_lifenum().has_value() && clone.refdata_lifenum().value() == 8,
                "reset of the original must not mutate a clone");

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
