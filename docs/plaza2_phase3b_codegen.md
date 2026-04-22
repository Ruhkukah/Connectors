# Phase 3B PLAZA II Metadata And Codegen

## Scope

Phase 3B adds the deterministic PLAZA II schema surface needed before any CGate
runtime work:

- a reviewed repo-local `.ini` fixture for the locked PLAZA II stream/table/field
  surface
- parser tooling for that reviewed `.ini`
- deterministic machine-readable metadata output
- deterministic C++-usable descriptor output
- offline validation for regeneration, schema drift, parser behavior, and private
  replication coverage

The produced module area is `protocols/plaza2_cgate/`.

## Non-goals

- no CGate runtime adapter
- no runtime probing
- no live connectivity
- no listener or connection wrappers
- no fake replication engine
- no replication state projection
- no persistence or replstate runtime logic
- no health/runtime snapshots
- no C ABI changes
- no `plaza2_trade`

## Input Schema Surface

Phase 3A intentionally did not commit local installed `forts_scheme.ini`. That
means Phase 3B cannot treat a runtime-installed vendor tree as the repo baseline
without bypassing the Phase 3A lock.

The authoritative Phase 3B inputs are therefore:

- `matrix/protocol_inventory/plaza2_streams.yaml`
- `matrix/protocol_inventory/plaza2_tables.yaml`
- locked `spec-lock/prod/plaza2/docs/cache/p2gate_en.html`

`tools/plaza2_schema_materialize.py` deterministically transcodes that locked
surface into the reviewed repo-local fixture:

- `protocols/plaza2_cgate/schema/plaza2_forts_reviewed.ini`
- `protocols/plaza2_cgate/schema/schema.manifest.json`

This is the only deliberate Phase 3B deviation from a direct vendor `.ini`
baseline, and it exists for one reason: the repo does not yet commit a locked
local `forts_scheme.ini`, while normal CI must remain offline-safe and vendor
runtime-free. The reviewed fixture keeps the phase deterministic, reviewable, and
anchored to the Phase 3A lock instead of to an untracked local install.

## Parser Architecture

There are two narrow tools:

- `tools/plaza2_schema_materialize.py`
  - reads the locked Phase 3A inventories plus locked `p2gate_en.html`
  - resolves field tables by stream/table anchor, with deterministic fallback for
    legacy anchor names still present in the docs
  - emits the reviewed `.ini` fixture and its manifest
- `tools/plaza2_schema_common.py`
  - parses the reviewed `.ini`
  - validates section structure and cross-references
  - normalizes primitive tokens
  - derives stable numeric ids using a fixed FNV-1a 32-bit algorithm
  - derives type metadata for integers, strings, decimals, timestamps, and binary
    payload families

The reviewed `.ini` is intentionally boring:

- `[meta]`
- `[stream:<stream>]`
- `[table:<stream>.<table>]`
- `[field:<stream>.<table>.<field>]`

That keeps the parser small, explicit, and deterministic.

## Generated Output Structure

`tools/plaza2_codegen.py` consumes the reviewed `.ini` and emits:

- `protocols/plaza2_cgate/generated/plaza2_generated_metadata.json`
- `protocols/plaza2_cgate/generated/plaza2_generated_metadata.hpp`
- `protocols/plaza2_cgate/generated/plaza2_generated_metadata.cpp`

The generated surface includes:

- stream descriptors
- table descriptors
- field descriptors
- primitive/type descriptors
- stable numeric ids for streams, tables, fields, and types
- generated C++ enums for `StreamCode`, `TableCode`, and `FieldCode`
- numeric descriptor helpers for:
  - `FindStreamByCode`
  - `FindTableByCode`
  - `FindFieldByCode`
  - `TablesForStream`
  - `FieldsForTable`

The C++ output is metadata-only. No vendor types appear above this boundary, and
no runtime adapter logic is mixed in.

## Deterministic Guarantees

Phase 3B determinism is enforced by construction:

- Phase 3A inventories define stream/table order
- field order comes from the locked doc table order
- ids come from a fixed string-to-FNV-1a mapping, not from hash-map order or
  ordinal assignment
- emitted files are written with fixed ordering and fixed formatting
- generated outputs contain no local absolute paths and no timestamps
- check mode compares committed outputs byte-for-byte

## Tests Added

- `plaza2_schema_materialize_check`
  - verifies the reviewed `.ini` fixture still regenerates exactly from the
    locked Phase 3A doc surface
- `plaza2_codegen_check`
  - verifies generated JSON/C++ outputs still regenerate exactly
- `plaza2_scheme_parser_check`
  - parser unit coverage for valid input, unsupported types, broken references,
    and duplicate sections
- `plaza2_metadata_invariants_check`
  - verifies locked Phase 3A stream/table coverage, required private-state-first
    coverage, and stable numeric ids
- `plaza2_generated_metadata_test`
  - C++ smoke test for generated descriptor access

## Safety Model

Phase 3B preserves the existing repo safety model:

- no live connectivity
- no vendor runtime required in normal CI
- no secrets
- no external session bring-up
- no unsafe defaults
- no C ABI expansion
- no trading path work

## Performance Considerations

This phase is codegen-only, but the output is shaped for the later 2–4 vCPU VPS
runtime target:

- generated numeric ids avoid string-heavy routing in later phases
- descriptor access is array/span-based
- there is no reflection-style heap-driven metadata layer
- stream/table/field groupings are already laid out for cheap index-based lookup
- strings remain for diagnostics and provenance, not as the primary dispatch key

## Known Limitations

- the reviewed `.ini` is a deterministic transcode of the locked docs, not yet a
  committed lock of the actual installed `forts_scheme.ini`
- decimal and timestamp descriptors carry precise type-category metadata but not
  final runtime decode semantics; those belong to later adapter/runtime phases
- legacy doc anchor naming still exists in a few places, so the materializer has
  a narrow fallback that resolves by stream section plus table summary

## Why This Fits The Lean Runtime Goal

The generated surface is intentionally boring:

- fixed numeric ids
- compile-time enums
- span-backed descriptors
- no dynamic registries
- no dependency on CGate at parse/codegen time

That is appropriate for later lean runtime use on a 2–4 vCPU colocated VPS
because it front-loads schema interpretation offline and leaves later runtime
phases with simple numeric metadata lookup instead of string-heavy reflection.

## Next Recommended Phase

Phase 3C: native CGate runtime adapter.
