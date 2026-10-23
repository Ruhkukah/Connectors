#pragma once
#include "moex/plaza2/cgate/plaza2_private_state_bridge.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
namespace moex::plaza2::cgate {
inline void private_instrument_update_regression() {
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
    private_state::Plaza2PrivateStateProjector projector;
    Plaza2PrivateStateBridge bridge(projector);
    const std::array streams{kFortsRefdataRepl, kFortsInstrumentstateRepl};
    check(!bridge.reset(streams) && !bridge.begin_run(), "instrument view begin run");
    const auto event = [&](auto kind, auto stream) {
        check(!bridge.on_plaza2_listener_event({.kind = kind, .stream_code = stream}), "instrument view lifecycle");
    };
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsRefdataRepl);
    for (int i = 1; i <= 41000; ++i) {
        const std::array fields{number(kFortsRefdataReplFutInstrumentsIsinId, i),
                                number(kFortsRefdataReplFutInstrumentsReplId, i)};
        check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                .stream_code = kFortsRefdataRepl,
                                                .table_code = kFortsRefdataReplFutInstruments,
                                                .fields = fields,
                                                .signed_value = 1}),
              "large instrument bootstrap row");
    }
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsRefdataRepl);
    check(projector.instruments().size() == 41000, "instrument view size");
    std::int64_t worst{};
    for (int phase : {1, 6, 2}) {
        const std::array fields{number(kFortsInstrumentstateReplInstrumentStateIsinId, 23000),
                                number(kFortsInstrumentstateReplInstrumentStateReplId, 23000),
                                number(kFortsInstrumentstateReplInstrumentStatePublicState, phase)};
        auto start = std::chrono::steady_clock::now();
        event(Plaza2ListenerEventKind::TransactionBegin, kFortsInstrumentstateRepl);
        check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                                .stream_code = kFortsInstrumentstateRepl,
                                                .table_code = kFortsInstrumentstateReplInstrumentState,
                                                .fields = fields,
                                                .signed_value = phase + 10}),
              "online instrument status");
        event(Plaza2ListenerEventKind::TransactionCommit, kFortsInstrumentstateRepl);
        worst = std::max<std::int64_t>(
            worst,
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        const auto rows = projector.instruments();
        check(rows.size() == 41000 && rows[22999].isin_id == 23000 && rows[22999].current_status == phase &&
                  !rows[22998].has_current_status && !rows[23000].has_current_status,
              "single instrument update changed unrelated sorted rows");
    }
    check(worst < 1000, "one online instrument row rebuilt 41k instrument views");
    event(Plaza2ListenerEventKind::TransactionBegin, kFortsInstrumentstateRepl);
    check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::ClearDeleted,
                                            .stream_code = kFortsInstrumentstateRepl,
                                            .table_code = kFortsInstrumentstateReplInstrumentState,
                                            .signed_value = 100}),
          "instrument status clear");
    const std::array replacement{number(kFortsInstrumentstateReplInstrumentStateIsinId, 23001),
                                 number(kFortsInstrumentstateReplInstrumentStateReplId, 23001),
                                 number(kFortsInstrumentstateReplInstrumentStatePublicState, 6)};
    check(!bridge.on_plaza2_listener_event({.kind = Plaza2ListenerEventKind::StreamData,
                                            .stream_code = kFortsInstrumentstateRepl,
                                            .table_code = kFortsInstrumentstateReplInstrumentState,
                                            .fields = replacement,
                                            .signed_value = 101}),
          "instrument status after clear");
    event(Plaza2ListenerEventKind::TransactionCommit, kFortsInstrumentstateRepl);
    check(!projector.instruments()[22999].has_current_status && projector.instruments()[23000].current_status == 6,
          "clear and upsert retained an unrelated retired status");
}
} // namespace moex::plaza2::cgate
