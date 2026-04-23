# Phase 3D: PLAZA II Fake Replication Engine

## Scope

Phase 3D adds a fully offline fake replication engine for `plaza2_repl` lifecycle
testing. This phase introduces:

- YAML-backed fake-engine scenarios under `tests/plaza2_cgate/scenarios/`
- deterministic materialization of those scenarios into generated C++ descriptors
- a thin native fake-engine runner that executes the scenario event sequence
- focused parser, lifecycle, and rule-enforcement tests

The Phase 3D surface is deliberately limited to replication lifecycle semantics.

## Non-goals

This phase does not add:

- live CGate connectivity
- runtime adapter changes
- replication state projection
- persistence
- business logic for orders, positions, or limits
- C ABI exports
- threading or background workers

## Architecture

The committed YAML files are the source of truth. They are validated and
materialized offline by `tools/plaza2_fake_scenario_materialize.py` against the
Phase 3B generated metadata surface.

The materializer emits:

- `protocols/plaza2_cgate/generated/plaza2_fake_scenarios.json`
- `protocols/plaza2_cgate/generated/plaza2_fake_scenarios.hpp`
- `protocols/plaza2_cgate/generated/plaza2_fake_scenarios.cpp`

The native runner lives in:

- `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_fake_engine.hpp`
- `protocols/plaza2_cgate/src/plaza2_fake_engine.cpp`

The runtime side does not parse YAML. It consumes deterministic generated
descriptors only. That keeps the fake engine hermetic, reviewable, and free of
new runtime dependencies.

## Scenario Model

Each scenario is:

- explicit stream declarations
- an explicit ordered event list
- explicit expected invariants

Supported event kinds in Phase 3D:

- `OPEN`
- `CLOSE`
- `SNAPSHOT_BEGIN`
- `SNAPSHOT_END`
- `ONLINE`
- `TN_BEGIN`
- `TN_COMMIT`
- `STREAM_DATA`
- `P2REPL_REPLSTATE`
- `P2REPL_LIFENUM`
- `P2REPL_CLEARDELETED`

The parser and the runner both enforce strict transaction ordering:

- `STREAM_DATA` requires an active transaction
- `TN_COMMIT` requires `TN_BEGIN`
- nested `TN_BEGIN` is rejected
- control events are rejected inside an open transaction

## State Model

The fake engine tracks only lifecycle-safe replication state:

- connector open/closed state
- snapshot-active flag
- online flag
- transaction-open flag
- total committed transaction count
- last seen lifenum
- last seen replstate string
- per-stream online state
- per-stream snapshot-complete state
- per-stream clear-deleted count
- per-stream committed row count
- callback error count

Life-number changes invalidate committed per-stream row state and clear the
stream online/snapshot-complete flags. This keeps the model aligned with later
resync handling without introducing the full state engine early.

## Safety Model

Phase 3D remains fully offline:

- no vendor runtime required
- no live network use
- no secrets
- no production endpoints
- no implicit timing or randomness

All scenario inputs are committed text fixtures. All generated outputs are
deterministic and checked in with `--check` validation.

## Performance Considerations

This phase is test-only, but it is still shaped for the later 2–4 vCPU target:

- no YAML parser in the native runner
- numeric generated stream/table/field descriptors from Phase 3B
- no hidden threads
- no reflection-heavy dispatch
- compact per-stream state and simple linear event replay

That keeps the fake engine cheap enough to use heavily in CI and later state
projection tests.

## Tests Added

- `plaza2_fake_scenario_materialize_check`
- `plaza2_fake_scenario_parser_check`
- `plaza2_fake_engine_scenario_test`
- `plaza2_fake_engine_rules_test`

Coverage includes:

- tracked YAML scenario parsing
- deterministic generated-output checking
- snapshot to online transition
- resume token handling
- lifenum reset invalidation
- clear-deleted handling
- invalid transaction ordering
- callback error containment

## Known Limitations

- Phase 3D validates row fields against the Phase 3B metadata surface, but it
  does not decode CGate payloads
- row content is tracked only as validated test descriptors, not as a business
  projection model
- invariant coverage is intentionally narrow in this phase
- scenario parsing is offline-only and tied to committed generated metadata

## Why This Fits The VPS Target

The fake engine is a rehearsal layer for later private-state work, not another
runtime service. By pushing YAML handling and schema validation into offline
materialization, the committed native surface stays lean and deterministic. That
avoids adding parser weight or concurrency complexity that would distort later
runtime work on a 2–4 vCPU colocated VPS.

## Next Recommended Phase

Phase 3E: private replication state engine on top of the fake-engine lifecycle
surface and the existing Phase 3B/3C metadata/runtime boundaries.
