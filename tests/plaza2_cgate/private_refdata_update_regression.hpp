#pragma once
#include "moex/plaza2/cgate/plaza2_private_state_bridge.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace moex::plaza2::cgate {
inline void private_refdata_update_regression() {
    using enum generated::FieldCode;
    using enum generated::StreamCode;
    using enum generated::TableCode;
    const auto check = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    const auto number = [](auto code, auto value) {
        return Plaza2DecodedFieldValue{
            .field_code = code, .kind = Plaza2DecodedValueKind::SignedInteger, .signed_value = value};
    };
    const auto text = [](auto code, std::string_view value) {
        return Plaza2DecodedFieldValue{.field_code = code, .kind = Plaza2DecodedValueKind::String, .text_value = value};
    };
    private_state::Plaza2PrivateStateProjector projector;
    Plaza2PrivateStateBridge bridge(projector);
    const std::array streams{kFortsRefdataRepl, kFortsInstrumentstateRepl, kFortsSessionstateRepl};
    check(!bridge.reset(streams) && !bridge.begin_run(), "REF view begin run");
    const auto event = [&](auto kind, auto stream) {
        check(!bridge.on_plaza2_listener_event({.kind = kind, .stream_code = stream}), "REF view lifecycle");
    };
    const auto row = [&](auto table, auto revision, auto fields) {
        check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                .stream_code = kFortsRefdataRepl,
                                                .table_code = table,
                                                .fields = fields,
                                                .signed_value = revision}),
              "REF row");
    };
    check(!bridge.on_plaza2_listener_event(
              {.kind = Plaza2ListenerEventKind::LifeNum, .stream_code = kFortsRefdataRepl, .unsigned_value = 1}),
          "REF epoch");
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsRefdataRepl);
    row(kFortsRefdataReplSession, 1,
        std::array{number(kFortsRefdataReplSessionSessId, 200), number(kFortsRefdataReplSessionState, 2)});
    row(kFortsRefdataReplFutVcb, 1,
        std::array{number(kFortsRefdataReplFutVcbReplId, 1), number(kFortsRefdataReplFutVcbBaseContractId, 50),
                   text(kFortsRefdataReplFutVcbBaseContractCode, "FUT"), text(kFortsRefdataReplFutVcbCurr, "RUB"),
                   text(kFortsRefdataReplFutVcbBoardMd, "RFUD")});
    for (int i = 1; i <= 41000; ++i)
        row(kFortsRefdataReplFutInstruments, 1,
            std::array{number(kFortsRefdataReplFutInstrumentsIsinId, i),
                       number(kFortsRefdataReplFutInstrumentsReplId, i),
                       text(kFortsRefdataReplFutInstrumentsBaseContractCode, "FUT")});
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsRefdataRepl);
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsSessionstateRepl);
    const std::array session_status{number(kFortsSessionstateReplSessionStateSessId, 200),
                                    number(kFortsSessionstateReplSessionStatePublicState, 1)};
    check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                            .stream_code = kFortsSessionstateRepl,
                                            .table_code = kFortsSessionstateReplSessionState,
                                            .fields = session_status,
                                            .signed_value = 2}),
          "current REF session");
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsSessionstateRepl);
    const auto membership = [&](auto price, auto session) {
        row(kFortsRefdataReplFutSessContents, 2,
            std::array{number(kFortsRefdataReplFutSessContentsIsinId, 23000),
                       number(kFortsRefdataReplFutSessContentsSessId, session),
                       number(kFortsRefdataReplFutSessContentsReplId, session),
                       text(kFortsRefdataReplFutSessContentsBaseContractCode, "FUT"),
                       text(kFortsRefdataReplFutSessContentsSettlementPrice, price)});
    };
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsRefdataRepl);
    membership("100", 200);
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsRefdataRepl);
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsInstrumentstateRepl);
    const std::array status_session{number(kFortsInstrumentstateReplSysEventsReplId, 1),
                                    number(kFortsInstrumentstateReplSysEventsSessId, 200)};
    check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                            .stream_code = kFortsInstrumentstateRepl,
                                            .table_code = kFortsInstrumentstateReplSysEvents,
                                            .fields = status_session,
                                            .signed_value = 1}),
          "REF status session");
    const std::array status{number(kFortsInstrumentstateReplInstrumentStateIsinId, 23000),
                            number(kFortsInstrumentstateReplInstrumentStatePublicState, 2)};
    check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                            .stream_code = kFortsInstrumentstateRepl,
                                            .table_code = kFortsInstrumentstateReplInstrumentState,
                                            .fields = status,
                                            .signed_value = 2}),
          "REF status");
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsInstrumentstateRepl);
    for (const auto stream : streams)
        event(Plaza2ListenerEventKind::Online, stream);
    std::array<std::int64_t, 3> online_samples{};
    bool same_publication = true;
    for (auto& sample : online_samples) {
        const auto* published = projector.instruments().data();
        for (const auto table : {kFortsRefdataReplSession, kFortsRefdataReplFutVcb, kFortsRefdataReplFutInstruments,
                                 kFortsRefdataReplFutSessContents, kFortsRefdataReplSysMessages})
            check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                                    .stream_code = kFortsRefdataRepl,
                                                    .table_code = table,
                                                    .signed_value = table == kFortsRefdataReplFutSessContents ? 2 : 1}),
                  "deferred no-op REF purge");
        const auto start = std::chrono::steady_clock::now();
        event(Plaza2ListenerEventKind::Online, kFortsRefdataRepl);
        sample = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        same_publication &= projector.instruments().data() == published;
        check(projector.find_instrument(23000)->current_status_refdata_bound &&
                  projector.find_instrument(23000)->settlement_price == "100" && projector.connector_health().online &&
                  !projector.connector_health().transaction_open,
              "no-op REF ONLINE changed committed definition, binding or health");
    }
    std::sort(online_samples.begin(), online_samples.end());
    std::cout << "41k deferred no-op REFDATA ONLINE median of 3: " << online_samples[1] / 1000.0 << " us\n";
    check(same_publication, "no-op deferred REF purge rematerialized all 41k committed instruments");
#if MOEX_RELEASE_PERFORMANCE_ACCEPTANCE
    check(online_samples[1] < 1000000, "no-op REF ONLINE blocks the owner on retained reference views");
#endif
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::array<std::int64_t, 3> session_samples{};
    same_publication = true;
    for (int i = 0; i < static_cast<int>(session_samples.size()); ++i) {
        const auto* published = projector.instruments().data();
        const auto generation = projector.status_binding_generation();
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsRefdataRepl);
        row(kFortsRefdataReplSession, 3 + i,
            std::array{number(kFortsRefdataReplSessionSessId, 200), number(kFortsRefdataReplSessionState, 3 + i),
                       number(kFortsRefdataReplSessionBegin, now - 60), number(kFortsRefdataReplSessionEnd, now + 3600),
                       number(kFortsRefdataReplSessionMarginCallFixSchedule, now + 600 + i)});
        check(projector.find_session(200)->state == (i ? 2 + i : 2) &&
                  projector.session_source_provenance(kFortsRefdataReplSession, 200)->repl_rev == (i ? 2 + i : 1),
              "session update or source revision leaked before commit");
        const auto start = std::chrono::steady_clock::now();
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsRefdataRepl);
        session_samples[i] =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        same_publication &= projector.instruments().data() == published;
        const auto* session = projector.find_session(200);
        check(session && session->state == 3 + i && session->begin == now - 60 && session->end == now + 3600 &&
                  session->margin_call_fix_schedule == now + 600 + i &&
                  projector.session_source_provenance(kFortsRefdataReplSession, 200)->repl_rev == 3 + i &&
                  projector.current_session_id(now) == 200 && projector.status_binding_generation() == generation &&
                  projector.find_instrument(23000)->current_status_refdata_bound &&
                  projector.connector_health().online && !projector.connector_health().transaction_open,
              "stable-session commit lost schedules, revision, binding or health");
    }
    std::sort(session_samples.begin(), session_samples.end());
    std::cout << "41k stable-session commit median of 3: " << session_samples[1] / 1000.0 << " us\n";
    check(same_publication, "stable-session commit rematerialized all 41k committed instruments");
#if MOEX_RELEASE_PERFORMANCE_ACCEPTANCE
    check(session_samples[1] < 1000000, "stable-session commit blocks the owner on retained instrument views");
#endif
    std::int64_t worst{};
    for (const int phase : {0, 1}) {
        const auto start = std::chrono::steady_clock::now();
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsRefdataRepl);
        if (!phase)
            row(kFortsRefdataReplFutInstruments, 3,
                std::array{number(kFortsRefdataReplFutInstrumentsIsinId, 23000),
                           number(kFortsRefdataReplFutInstrumentsReplId, 23000),
                           text(kFortsRefdataReplFutInstrumentsBaseContractCode, "FUT"),
                           text(kFortsRefdataReplFutInstrumentsSettlementPrice, "101")});
        else
            membership("102", 200);
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsRefdataRepl);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        worst = std::max(worst, elapsed);
        const auto views = projector.instruments();
        check(views.size() == 41000 && views[22999].settlement_price == (phase ? "102" : "101") &&
                  views[22999].sess_id == 200 && views[22999].current_session_member &&
                  views[22999].has_current_status && views[22999].current_status_refdata_bound &&
                  views[22999].future_vcb_join_status == private_state::FutureVcbJoinStatus::Resolved &&
                  views[22999].future_vcb_currency == "RUB" && views[22999].future_vcb_board_md == "RFUD" &&
                  views[22998].settlement_price.empty() && views[23000].settlement_price.empty(),
              "single REF update changed membership, VCB joins, status binding or unrelated instruments");
        const auto* indexed = projector.find_instrument(23000);
        check(indexed && indexed->sess_id == views[22999].sess_id &&
                  indexed->settlement_price == views[22999].settlement_price &&
                  indexed->current_session_member == views[22999].current_session_member &&
                  indexed->current_status_refdata_bound == views[22999].current_status_refdata_bound &&
                  indexed->future_vcb_currency == views[22999].future_vcb_currency &&
                  indexed->future_vcb_join_status == views[22999].future_vcb_join_status,
              "indexed instrument lost committed membership, VCB joins or current terms");
    }
    std::cout << "41k single-row REFDATA update worst: " << worst << " us\n";
#if MOEX_RELEASE_PERFORMANCE_ACCEPTANCE
    check(worst < 1000, "one online REFDATA row rebuilt 41k retained futures views");
#endif
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsRefdataRepl);
    membership("999", 201);
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsRefdataRepl);
    check(projector.instruments()[22999].sess_id == 200 && projector.instruments()[22999].settlement_price == "102",
          "future-session update replaced current selected terms");
    const auto next_session = [&](auto revision, auto begin, auto end) {
        row(kFortsRefdataReplSession, revision,
            std::array{number(kFortsRefdataReplSessionSessId, 201), number(kFortsRefdataReplSessionState, 2),
                       number(kFortsRefdataReplSessionBegin, begin), number(kFortsRefdataReplSessionEnd, end)});
    };
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsRefdataRepl);
    next_session(6, now + 7200, now + 10800);
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsRefdataRepl);
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsSessionstateRepl);
    const std::array next_status{number(kFortsSessionstateReplSessionStateSessId, 201),
                                 number(kFortsSessionstateReplSessionStatePublicState, 0)};
    check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                            .stream_code = kFortsSessionstateRepl,
                                            .table_code = kFortsSessionstateReplSessionState,
                                            .fields = next_status,
                                            .signed_value = 3}),
          "next session status");
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsSessionstateRepl);
    const auto generation = projector.status_binding_generation();
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsRefdataRepl);
    next_session(7, now - 60, now + 3600);
    check(projector.current_session_id(now) == 200 && projector.find_instrument(23000)->sess_id == 200 &&
              projector.find_session(201)->begin == now + 7200,
          "selected session changed before its schedule committed");
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsRefdataRepl);
    const auto* selected = projector.find_instrument(23000);
    check(projector.current_session_id(now) == 201 && selected->sess_id == 201 && selected->settlement_price == "999" &&
              selected->current_session_member && !selected->current_status_refdata_bound &&
              projector.status_binding_generation() > generation &&
              projector.instruments()[22999].settlement_price == "999" &&
              !projector.instruments()[22999].current_status_refdata_bound,
          "changed session schedule skipped selected terms or retained old status authority");
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsRefdataRepl);
    next_session(8, now + 7200, now + 10800);
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsRefdataRepl);
    check(projector.current_session_id(now) == 200 && projector.find_instrument(23000)->sess_id == 200 &&
              projector.find_instrument(23000)->settlement_price == "102" &&
              projector.find_instrument(23000)->current_status_refdata_bound,
          "restored session selection lost independently committed terms/status");
    (void)projector.take_row_changes();
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsRefdataRepl);
    row(kFortsRefdataReplFutInstruments, 4,
        std::array{number(kFortsRefdataReplFutInstrumentsReplId, 42),
                   number(kFortsRefdataReplFutInstrumentsReplAct, 1)});
    row(kFortsRefdataReplFutInstruments, 4,
        std::array{number(kFortsRefdataReplFutInstrumentsReplId, 23001),
                   number(kFortsRefdataReplFutInstrumentsIsinId, 23001),
                   text(kFortsRefdataReplFutInstrumentsBaseContractCode, "FUT"),
                   text(kFortsRefdataReplFutInstrumentsSettlementPrice, "103")});
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsRefdataRepl);
    const auto views = projector.instruments();
    check(views.size() == 40999 &&
              std::none_of(views.begin(), views.end(), [](const auto& value) { return value.isin_id == 42; }) &&
              views[22999].isin_id == 23001 && views[22999].settlement_price == "103" &&
              views[22998].isin_id == 23000 && views[22998].settlement_price == "102",
          "mixed sparse REF deletion and upsert retained a deleted sorted view slot");
    check(!projector.find_instrument(42) && projector.find_instrument(23001)->settlement_price == "103",
          "indexed mixed REF deletion and update disagreed with committed views");
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsRefdataRepl);
    row(kFortsRefdataReplFutInstruments, 5,
        std::array{number(kFortsRefdataReplFutInstrumentsReplId, 43),
                   number(kFortsRefdataReplFutInstrumentsIsinId, 42000),
                   text(kFortsRefdataReplFutInstrumentsBaseContractCode, "FUT"),
                   text(kFortsRefdataReplFutInstrumentsSettlementPrice, "104")});
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsRefdataRepl);
    const auto renamed = projector.instruments();
    check(renamed.size() == 40999 && renamed.back().isin_id == 42000 && renamed.back().settlement_price == "104" &&
              std::none_of(renamed.begin(), renamed.end(), [](const auto& value) { return value.isin_id == 43; }),
          "physical REF row rekey retained its old natural instrument view");
    const auto changes = projector.take_row_changes();
    check(!changes.resync_required && !changes.regular_trade_history_truncated && changes.orders.empty() &&
              changes.trades.empty(),
          "reference definition deletion/rekey invalidated regular private trading history");
    check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                            .stream_code = kFortsRefdataRepl,
                                            .table_code = kFortsRefdataReplFutInstruments,
                                            .signed_value = 2}),
          "real deferred REF purge");
    check(projector.instruments().size() == 40999, "deferred REF purge leaked ahead of ONLINE");
    event(Plaza2ListenerEventKind::Online, kFortsRefdataRepl);
    check(projector.instruments().size() == 3 && projector.find_instrument(23000) && projector.find_instrument(23001) &&
              projector.find_instrument(42000) && !projector.find_instrument(44),
          "purge fast path ignored real older source rows");
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsRefdataRepl);
    row(kFortsRefdataReplSession, 6,
        std::array{number(kFortsRefdataReplSessionSessId, 200), number(kFortsRefdataReplSessionState, 6),
                   number(kFortsRefdataReplSessionBegin, now - 60), number(kFortsRefdataReplSessionEnd, now + 3600)});
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsRefdataRepl);
    check(projector.instruments().size() == 3 && !projector.find_instrument(42) && !projector.find_instrument(44) &&
              projector.find_instrument(23000)->sess_id == 200 &&
              projector.find_instrument(23000)->settlement_price == "102",
          "session selection resurrected purged definitions or lost retained membership");
    check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                            .stream_code = kFortsRefdataRepl,
                                            .table_code = kFortsRefdataReplFutInstruments,
                                            .signed_value = std::numeric_limits<std::int64_t>::max()}),
          "clear-all REF definitions");
    event(Plaza2ListenerEventKind::Online, kFortsRefdataRepl);
    check(projector.instruments().size() == 1 && projector.find_instrument(23000)->current_session_member &&
              !projector.find_instrument(23001),
          "clear-all REF skipped deletion or removed independent membership/status");
    check(!bridge.on_plaza2_listener_event(
              {.kind = Plaza2ListenerEventKind::LifeNum, .stream_code = kFortsRefdataRepl, .unsigned_value = 2}),
          "REF life change after no-op purges");
    check(projector.instruments().size() == 1 && !projector.find_instrument(23000)->current_session_member &&
              !projector.find_instrument(23000)->current_status_refdata_bound && !projector.connector_health().online,
          "no-op purge optimization retained reference authority across actual LifeNum loss");
}
} // namespace moex::plaza2::cgate
