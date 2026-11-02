#pragma once

#include "moex/plaza2/cgate/plaza2_fake_engine.hpp"
#include "moex/plaza2/cgate/plaza2_private_state.hpp"

#include <initializer_list>
#include <limits>
#include <stdexcept>

namespace moex::plaza2::test::instrument_session {
using namespace generated;
using namespace projection;

inline FieldValueSpec integer(FieldCode code, std::int64_t value) {
    return {.field_code = code, .kind = ValueKind::kSignedInteger, .signed_value = value};
}

struct Harness {
    private_state::Plaza2PrivateStateProjector projector;
    EngineState state{
        .streams = {{.stream_code = StreamCode::kFortsInstrumentstateRepl, .online = true, .snapshot_complete = true}}};
    bool native{false};
    void begin(StreamCode stream) {
        state.transaction_open = true;
        projector.on_event(
            {}, {.kind = EventKind::kTransactionBegin, .stream_code = stream, .numeric_value = native ? 1U : 0U},
            state);
    }
    void row(TableCode table, std::int64_t revision, std::initializer_list<FieldValueSpec> fields) {
        const auto stream = static_cast<StreamCode>(FindTableByCode(table)->stream_id);
        const EventSpec event{
            .kind = EventKind::kStreamData, .stream_code = stream, .table_code = table, .signed_value = revision};
        projector.on_stream_row({}, event, {.stream_code = stream, .table_code = table}, fields, state);
    }
    void commit(StreamCode stream) {
        state.transaction_open = false;
        ++state.commit_count;
        const EventSpec event{.kind = EventKind::kTransactionCommit, .stream_code = stream};
        projector.on_transaction_commit({}, event, state);
    }
    void member(std::int32_t session, std::int64_t revision = 10) {
        begin(StreamCode::kFortsRefdataRepl);
        row(TableCode::kFortsRefdataReplFutSessContents, revision,
            {integer(FieldCode::kFortsRefdataReplFutSessContentsIsinId, 1001),
             integer(FieldCode::kFortsRefdataReplFutSessContentsSessId, session),
             integer(FieldCode::kFortsRefdataReplFutSessContentsReplAct, 0)});
        commit(StreamCode::kFortsRefdataRepl);
    }
    void status(std::int64_t revision = 20) {
        row(TableCode::kFortsInstrumentstateReplInstrumentState, revision,
            {integer(FieldCode::kFortsInstrumentstateReplInstrumentStateIsinId, 1001),
             integer(FieldCode::kFortsInstrumentstateReplInstrumentStatePublicState, 1)});
    }
    void session(std::int32_t session, std::int64_t revision = 21) {
        row(TableCode::kFortsInstrumentstateReplSysEvents, revision,
            {integer(FieldCode::kFortsInstrumentstateReplSysEventsReplId, revision),
             integer(FieldCode::kFortsInstrumentstateReplSysEventsSessId, session),
             integer(FieldCode::kFortsInstrumentstateReplSysEventsEventId, revision)});
    }
    void clear(TableCode table, std::int64_t revision) {
        projector.on_event({},
                           {.kind = EventKind::kClearDeleted,
                            .stream_code = StreamCode::kFortsInstrumentstateRepl,
                            .table_code = table,
                            .signed_value = revision},
                           state);
    }
    bool bound() const {
        const auto rows = projector.instruments();
        const auto* indexed = projector.find_instrument(1001);
        if ((indexed != nullptr) != !rows.empty() ||
            (indexed && (indexed->sess_id != rows.front().sess_id ||
                         indexed->has_current_status != rows.front().has_current_status ||
                         indexed->current_status != rows.front().current_status ||
                         indexed->current_status_refdata_bound != rows.front().current_status_refdata_bound)))
            throw std::runtime_error("indexed instrument disagrees with the committed view");
        return indexed && indexed->current_status_refdata_bound;
    }
};

inline void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

inline void run() {
    for (const bool native : {false, true}) {
        Harness indexed;
        indexed.native = native;
        require(!indexed.projector.find_instrument(1001) && !indexed.projector.find_session(321),
                "indexed getters exposed missing rows");
        indexed.begin(StreamCode::kFortsRefdataRepl);
        indexed.row(TableCode::kFortsRefdataReplSession, 1,
                    {integer(FieldCode::kFortsRefdataReplSessionReplId, 1),
                     integer(FieldCode::kFortsRefdataReplSessionSessId, 321)});
        indexed.row(TableCode::kFortsRefdataReplFutSessContents, 2,
                    {integer(FieldCode::kFortsRefdataReplFutSessContentsReplId, 2),
                     integer(FieldCode::kFortsRefdataReplFutSessContentsIsinId, 1001),
                     integer(FieldCode::kFortsRefdataReplFutSessContentsSessId, 321)});
        require(!indexed.projector.find_instrument(1001) && !indexed.projector.find_session(321),
                "indexed getters published initial rows before commit");
        indexed.commit(StreamCode::kFortsRefdataRepl);
        indexed.begin(StreamCode::kFortsSessionstateRepl);
        indexed.row(TableCode::kFortsSessionstateReplSessionState, 3,
                    {integer(FieldCode::kFortsSessionstateReplSessionStateSessId, 321),
                     integer(FieldCode::kFortsSessionstateReplSessionStatePublicState, 1)});
        require(!indexed.projector.find_session(321)->has_current_status,
                "indexed session published uncommitted status");
        indexed.commit(StreamCode::kFortsSessionstateRepl);
        auto clone = indexed.projector.clone();
        require(clone.find_instrument(1001) != indexed.projector.find_instrument(1001) &&
                    clone.find_session(321) != indexed.projector.find_session(321),
                "indexed clone borrowed source map pointers");
        indexed.begin(StreamCode::kFortsSessionstateRepl);
        indexed.row(TableCode::kFortsSessionstateReplSessionState, 4,
                    {integer(FieldCode::kFortsSessionstateReplSessionStateSessId, 321),
                     integer(FieldCode::kFortsSessionstateReplSessionStatePublicState, 0)});
        require(indexed.projector.find_session(321)->current_status == 1,
                "indexed session did not retain committed status during native staging");
        indexed.commit(StreamCode::kFortsSessionstateRepl);
        require(indexed.projector.find_session(321)->current_status == 0 &&
                    clone.find_session(321)->current_status == 1,
                "indexed session commit changed an independent clone");
        indexed.begin(StreamCode::kFortsRefdataRepl);
        indexed.row(TableCode::kFortsRefdataReplFutSessContents, 5,
                    {integer(FieldCode::kFortsRefdataReplFutSessContentsReplId, 2),
                     integer(FieldCode::kFortsRefdataReplFutSessContentsReplAct, 1)});
        require(indexed.projector.find_instrument(1001)->current_session_member,
                "indexed instrument applied pending deletion");
        indexed.commit(StreamCode::kFortsRefdataRepl);
        require(!indexed.projector.find_instrument(1001), "indexed instrument retained deleted membership");
        indexed.projector.reset();
        require(!indexed.projector.find_session(321) && clone.find_instrument(1001)->current_session_member,
                "indexed reset retained a row or invalidated the clone");
        Harness h;
        h.native = native;
        h.member(321);
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.status();
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(!h.bound(), "instrument status was bound without committed INSTRUMENTSTATE sys_events session");
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.session(321);
        require(!h.bound(), "uncommitted instrument session marker authorized status");
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(h.bound(), "committed same-epoch session marker did not bind preceding status");

        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.session(322, 22);
        require(h.bound(), "pending session rollover changed the committed status binding");
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(!h.bound(), "mismatching stream session authorized current REFDATA membership");
        h.member(322, 11);
        require(!h.bound(), "new session membership reused an old status row");
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.status(23);
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(h.bound(), "fresh status in the matching session epoch did not bind");
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.session(322, 24);
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(h.bound(), "same-session event incorrectly invalidated status epoch");
        h.clear(TableCode::kFortsInstrumentstateReplSysEvents, 24);
        require(h.bound(), "clear-deleted removed a retained latest session marker");
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.clear(TableCode::kFortsInstrumentstateReplSysEvents, 25);
        require(h.bound(), "uncommitted clear-deleted invalidated the committed session marker");
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(!h.bound(), "clear-deleted retained a removed session authority");
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.session(322, 26);
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(!h.bound(), "a restored session marker reused status from before clear-deleted");
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.status(27);
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(h.bound(), "fresh status after marker reconstruction did not bind");
        h.projector.reset_status_snapshot(StreamCode::kFortsInstrumentstateRepl);
        require(!h.bound(), "status snapshot reopen retained old session authority");
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.status(28);
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(!h.bound(), "status alone re-established authority after snapshot reopen");
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.session(322, 29);
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(h.bound(), "matching reconstructed status and session did not bind");
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.row(TableCode::kFortsInstrumentstateReplSysEvents, 30,
              {integer(FieldCode::kFortsInstrumentstateReplSysEventsReplId, 29),
               integer(FieldCode::kFortsInstrumentstateReplSysEventsReplAct, 1)});
        require(h.bound(), "uncommitted marker deletion changed the committed binding");
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(!h.bound(), "deleted latest sys_events marker retained instrument session authority");
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.session(322, 31);
        h.status(32);
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(h.bound(), "session-before-status in one committed transaction did not bind");
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.clear(TableCode::kFortsInstrumentstateReplSysEvents, std::numeric_limits<std::int64_t>::max());
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(!h.bound(), "clear-all retained session authority from the prior revision epoch");
        h.begin(StreamCode::kFortsInstrumentstateRepl);
        h.session(322, 1);
        h.status(2);
        h.commit(StreamCode::kFortsInstrumentstateRepl);
        require(h.bound(), "clear-all blocked a rebuilt marker whose native revisions restarted");
        h.projector.on_event(
            {}, {.kind = EventKind::kLifeNum, .stream_code = StreamCode::kFortsInstrumentstateRepl, .numeric_value = 1},
            h.state);
        h.projector.on_event(
            {}, {.kind = EventKind::kLifeNum, .stream_code = StreamCode::kFortsInstrumentstateRepl, .numeric_value = 2},
            h.state);
        require(!h.bound(), "instrument LifeNum rollover retained old status binding");

        Harness before_member;
        before_member.native = native;
        before_member.begin(StreamCode::kFortsInstrumentstateRepl);
        before_member.status();
        before_member.session(321);
        before_member.commit(StreamCode::kFortsInstrumentstateRepl);
        require(!before_member.bound(), "instrument session marker bypassed REFDATA membership");
        before_member.member(321);
        require(before_member.bound(), "matching committed status-before-membership did not bind");
        before_member.projector.on_event(
            {}, {.kind = EventKind::kClose, .stream_code = StreamCode::kFortsInstrumentstateRepl}, before_member.state);
        require(!before_member.bound(), "disconnect retained old instrument session binding");

        Harness unordered;
        unordered.native = native;
        unordered.member(321);
        unordered.begin(StreamCode::kFortsInstrumentstateRepl);
        unordered.session(322, 100);
        unordered.session(321, 99);
        unordered.status(101);
        unordered.commit(StreamCode::kFortsInstrumentstateRepl);
        require(!unordered.bound(), "unordered historical session marker rolled current authority backward");
        unordered.member(322, 11);
        require(unordered.bound(), "latest committed marker did not survive unordered snapshot history");
        unordered.begin(StreamCode::kFortsInstrumentstateRepl);
        unordered.row(TableCode::kFortsInstrumentstateReplSysEvents, 102,
                      {integer(FieldCode::kFortsInstrumentstateReplSysEventsReplId, 100),
                       integer(FieldCode::kFortsInstrumentstateReplSysEventsReplAct, 1)});
        unordered.commit(StreamCode::kFortsInstrumentstateRepl);
        require(!unordered.bound(), "unordered history displaced the latest marker deletion identity");
        unordered.begin(StreamCode::kFortsInstrumentstateRepl);
        unordered.session(321, 99);
        unordered.status(103);
        unordered.commit(StreamCode::kFortsInstrumentstateRepl);
        require(!unordered.bound(), "deleted latest marker was replaced by lower revision history");

        Harness initial_history;
        initial_history.native = native;
        initial_history.state.streams.front().online = false;
        initial_history.state.streams.front().snapshot_complete = false;
        initial_history.member(322);
        initial_history.begin(StreamCode::kFortsInstrumentstateRepl);
        initial_history.status(20);
        initial_history.session(321, 21);
        initial_history.session(322, 22);
        initial_history.commit(StreamCode::kFortsInstrumentstateRepl);
        initial_history.state.streams.front().online = true;
        initial_history.state.streams.front().snapshot_complete = true;
        initial_history.projector.on_event(
            {}, {.kind = EventKind::kOnline, .stream_code = StreamCode::kFortsInstrumentstateRepl},
            initial_history.state);
        require(initial_history.bound(), "fresh snapshot status before session history failed to bind latest marker");
    }
}
} // namespace moex::plaza2::test::instrument_session
