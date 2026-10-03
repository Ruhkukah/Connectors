#pragma once
#include "moex/plaza2/cgate/plaza2_private_state_bridge.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
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
    const std::array status{number(kFortsInstrumentstateReplInstrumentStateIsinId, 23000),
                            number(kFortsInstrumentstateReplInstrumentStatePublicState, 2)};
    check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                            .stream_code = kFortsInstrumentstateRepl,
                                            .table_code = kFortsInstrumentstateReplInstrumentState,
                                            .fields = status,
                                            .signed_value = 2}),
          "REF status");
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsInstrumentstateRepl);
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
}
} // namespace moex::plaza2::cgate
