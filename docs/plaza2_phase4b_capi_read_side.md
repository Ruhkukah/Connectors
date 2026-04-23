# Phase 4B: PLAZA II TWIME C API Read Side

## Scope

Phase 4B adds a strict read-only C ABI over the already committed native snapshot surfaces from:

- Phase 3E private replication state
- Phase 4A PLAZA II + TWIME reconciler

The implementation extends the existing central C ABI surface in:

- `include/adapters/alorengine_capi/moex_c_api.h`
- `src/phase0_core.cpp`

This phase exports committed snapshot reads only. It does not add write-side order entry or live default behavior.

## Non-goals

Phase 4B does not add:

- write-side or order-entry C ABI
- default live order placement
- `plaza2_trade`
- public market-data C ABI
- persistence or journaling
- transport or socket work
- combined live PLAZA + TWIME orchestration
- hidden threads, worker pools, or async runtime
- broad ABI redesign outside the existing repo C ABI style

## ABI Style Used

The read side preserves the repo’s existing C ABI conventions:

- `extern "C"` entry points only
- fixed-width integer types only
- no C++ types in public headers
- `struct_size` on summary getter structs
- append-only enums and status codes
- fixed-size character buffers for text fields
- explicit status-code returns
- no exceptions crossing the ABI boundary

The public ABI remains C-safe and does not return borrowed pointers to internal STL-backed storage.

## Ownership Rules

The read-side ownership model is explicit:

- callers own all output buffers
- count queries report committed item counts only
- copy-out calls write into caller-provided arrays
- public ABI functions never return raw pointers into internal native vectors
- reads have no refresh side effects and do not mutate committed native state

For offline tests only, the repo uses narrow internal C++ install hooks in `src/moex_c_api_internal.hpp` to attach committed projector/reconciler objects to a connector handle. That hook is not part of the public C ABI contract.

## Count / Copy-Out Pattern

The exported domains use the preferred explicit access pattern:

- summary getters for singleton domains
- count query per repeated domain
- copy-out per repeated domain into caller-owned buffers

Representative shape:

- `*_get_*_health(...)`
- `*_get_*_count(...)`
- `*_copy_*_items(..., buffer, capacity, written)`

Capacity behavior is deterministic:

1. count queries return the committed item count
2. copy-out calls return `MOEX_RESULT_BUFFER_TOO_SMALL` when `capacity < required_count`
3. on `BUFFER_TOO_SMALL`, `written` is set to the required count
4. no partial copy is performed on insufficient capacity
5. null pointer and invalid-argument handling is explicit and stable

## Exported Domains

### Private replication read side

Singleton getters:

- `moex_get_plaza2_private_connector_health(...)`
- `moex_get_plaza2_resume_markers(...)`

Count + copy-out domains:

- stream health
- trading sessions
- instruments
- matching map
- limits
- positions
- own orders
- own trades

### Reconciler read side

Singleton getters:

- `moex_get_plaza2_twime_reconciler_health(...)`

Count + copy-out domains:

- reconciled orders
- reconciled trades

These exports carry committed essential fields only. They do not dump internal native structs blindly.

## Error / Status Model

The read-side ABI uses explicit `MoexResult` status codes:

- `MOEX_RESULT_OK`
- `MOEX_RESULT_INVALID_ARGUMENT`
- `MOEX_RESULT_NULL_POINTER`
- `MOEX_RESULT_BUFFER_TOO_SMALL`
- `MOEX_RESULT_SNAPSHOT_UNAVAILABLE`
- `MOEX_RESULT_OVERFLOW`
- `MOEX_RESULT_TRANSLATION_FAILED`
- existing lifecycle/status codes remain available for the broader ABI surface

Semantics:

- `NULL_POINTER` is returned for required null output pointers
- `INVALID_ARGUMENT` is returned for bad `struct_size` or similar caller contract violations
- `BUFFER_TOO_SMALL` is returned when the caller capacity is insufficient
- `SNAPSHOT_UNAVAILABLE` is returned when the handle has no committed private-state or reconciler snapshot attached
- `TRANSLATION_FAILED` is reserved for unexpected translation failures caught inside the ABI boundary

## Compatibility / Versioning Notes

- `MOEX_C_ABI_VERSION` remains append-only
- the Phase 4B additions are non-breaking extensions to the existing C ABI surface
- new enums and functions are additive
- summary structs carry `struct_size`
- callers must provide `struct_size >= sizeof(current_known_struct)` for summary getters

The ABI does not use unstable best-effort layout assumptions.

## Committed Snapshot Semantics

Phase 4B reads committed snapshots only.

That means:

- no staged transaction state is exposed
- no mutable iterators are exported
- no partial transaction visibility is allowed
- no read call triggers an implicit refresh or recomputation side effect

The C ABI translates from the committed native projector and reconciler read side only.

## Tests Added

Updated:

- `tests/phase0_selfcheck.cpp`
  - verifies size and alignment for the new Phase 4B public structs

Added:

- `tests/phase4b_capi_read_side_test.cpp`

The new test proves:

1. committed snapshot counts are retrievable deterministically
2. copy-out works safely into caller-owned buffers
3. insufficient-capacity behavior is explicit and non-partial
4. null and invalid arguments are handled safely
5. repeated reads do not mutate committed native state
6. reconciler exports preserve divergence/fault status
7. no live runtime or external connectivity is required

Regression coverage run alongside Phase 4B:

- `phase0_selfcheck`
- `moex_phase4b_capi_read_side_test`
- `plaza2_private_state_projection_test`
- `plaza2_private_state_visibility_test`
- `plaza2_private_state_invalidation_test`
- `plaza2_twime_reconciler_normalization_test`
- `plaza2_twime_reconciler_orders_test`
- `plaza2_twime_reconciler_trades_test`
- `plaza2_twime_reconciler_invalidation_test`

## Known Limitations

- The public read-side ABI is implemented, but live runtime attachment of the committed projector/reconciler onto the connector handle is still deferred to a later phase.
- Text fields use fixed-size buffers and may truncate overlong internal strings deterministically.
- The read side exports committed essential fields, not every internal native field.
- No write-side or public market-data ABI exists in this phase.

## Why This Stays Suitable for a Lean 2–4 vCPU VPS

The design remains suitable for a lean colocated runtime because it:

- stays single-threaded on the read path
- uses explicit caller-owned buffers instead of heap-heavy borrowed views
- avoids hidden background allocators or refresh workers
- translates from already committed in-memory state only
- keeps the ABI scalar-heavy and predictable for downstream managed consumers
- preserves offline-safe CI and deterministic replay/testing posture

This keeps the future runtime cost centered on the already approved native state engines, not on the ABI layer.

## Next Recommended Phase

The next recommended phase is the approved write-side / higher-level orchestration work that consumes this read-only ABI surface without weakening the current no-live-defaults and no-public-market-data constraints.
