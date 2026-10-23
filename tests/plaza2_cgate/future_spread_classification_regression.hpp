#pragma once

#include "fake_cgate_control.hpp"

namespace moex::plaza2::test::spread_classification {

// Feed the real CGate decoder, rather than modifying the projected snapshot.
// The caller deliberately supplies TN_COMMIT later to exercise visibility.
inline void enqueue_catalog_row(const fake::Control& runtime, bool participates, std::int32_t signs, std::int32_t mode,
                                std::int64_t revision) {
    using enum generated::StreamCode;
    using enum generated::TableCode;
    using enum generated::FieldCode;
    runtime.enqueue({.kind = fake::EventKind::Begin, .stream_code = kFortsRefdataRepl});
    runtime.enqueue(
        {.stream_code = kFortsRefdataRepl,
         .table_code = kFortsRefdataReplFutInstruments,
         .revision = revision,
         .fields = {
             {.field_code = kFortsRefdataReplFutInstrumentsReplId, .signed_value = 1001},
             {.field_code = kFortsRefdataReplFutInstrumentsReplRev, .signed_value = revision},
             {.field_code = kFortsRefdataReplFutInstrumentsIsinId, .signed_value = 1001},
             {.field_code = kFortsRefdataReplFutInstrumentsIsin, .kind = fake::FieldKind::Text, .text = "RTS-6.26"},
             {.field_code = kFortsRefdataReplFutInstrumentsShortIsin, .kind = fake::FieldKind::Text, .text = "RIH6"},
             {.field_code = kFortsRefdataReplFutInstrumentsName,
              .kind = fake::FieldKind::Text,
              .text = "spread participation regression"},
             {.field_code = kFortsRefdataReplFutInstrumentsBaseContractCode,
              .kind = fake::FieldKind::Text,
              .text = "RTS"},
             {.field_code = kFortsRefdataReplFutInstrumentsInstTerm, .signed_value = 3},
             {.field_code = kFortsRefdataReplFutInstrumentsRoundto, .signed_value = 2},
             {.field_code = kFortsRefdataReplFutInstrumentsLotVolume, .signed_value = 1},
             {.field_code = kFortsRefdataReplFutInstrumentsTradeModeId, .signed_value = mode},
             {.field_code = kFortsRefdataReplFutInstrumentsSigns, .signed_value = signs},
             {.field_code = kFortsRefdataReplFutInstrumentsIsSpread, .signed_value = participates},
             {.field_code = kFortsRefdataReplFutInstrumentsMinStep, .kind = fake::FieldKind::Text, .text = "250"},
             {.field_code = kFortsRefdataReplFutInstrumentsStepPrice, .kind = fake::FieldKind::Text, .text = "12.5"},
             {.field_code = kFortsRefdataReplFutInstrumentsStepPriceCurr,
              .kind = fake::FieldKind::Text,
              .text = "12.5"},
             {.field_code = kFortsRefdataReplFutInstrumentsSettlementPrice,
              .kind = fake::FieldKind::Text,
              .text = "102500"},
             {.field_code = kFortsRefdataReplFutInstrumentsGroupMask, .signed_value = 1},
             {.field_code = kFortsRefdataReplFutInstrumentsTradePeriodAccess, .signed_value = 1}}});
}

inline void commit_and_refresh_status(const fake::Control& runtime, std::int64_t revision) {
    using enum generated::StreamCode;
    using enum generated::TableCode;
    using enum generated::FieldCode;
    runtime.enqueue({.kind = fake::EventKind::Commit, .stream_code = kFortsRefdataRepl});
    runtime.enqueue({.kind = fake::EventKind::Begin, .stream_code = kFortsInstrumentstateRepl});
    runtime.enqueue({.stream_code = kFortsInstrumentstateRepl,
                     .table_code = kFortsInstrumentstateReplSysEvents,
                     .revision = revision + 1,
                     .fields = {{.field_code = kFortsInstrumentstateReplSysEventsReplId, .signed_value = revision + 1},
                                {.field_code = kFortsInstrumentstateReplSysEventsSessId, .signed_value = 321}}});
    runtime.enqueue(
        {.stream_code = kFortsInstrumentstateRepl,
         .table_code = kFortsInstrumentstateReplInstrumentState,
         .revision = revision + 2,
         .fields = {{.field_code = kFortsInstrumentstateReplInstrumentStateReplId, .signed_value = 1001},
                    {.field_code = kFortsInstrumentstateReplInstrumentStateIsinId, .signed_value = 1001},
                    {.field_code = kFortsInstrumentstateReplInstrumentStatePublicState, .signed_value = 1}}});
    runtime.enqueue({.kind = fake::EventKind::Commit, .stream_code = kFortsInstrumentstateRepl});
}

} // namespace moex::plaza2::test::spread_classification
