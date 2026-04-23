# PLAZA II Phase 3E Private Replication State

## Scope

Phase 3E adds the first operationally meaningful `plaza2_repl` state engine.

This phase implements:

- normalized in-memory private-state domains
- deterministic numeric `TableCode`-based row projection
- explicit staging between `TN_BEGIN` and `TN_COMMIT`
- explicit committed read-only snapshots
- per-stream in-memory health and watermark tracking
- fake-engine-driven deterministic replay tests

The implementation lives in:

- `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_private_state.hpp`
- `protocols/plaza2_cgate/src/plaza2_private_state.cpp`

## Non-Goals

Phase 3E does not add:

- live CGate bring-up
- session runners
- persistence or on-disk replstate journals
- C ABI exports
- TWIME reconciliation
- public aggregated market data
- public ordlog or order-book profiles
- `plaza2_trade`
- background threads or asynchronous workers

## Normalized State Domains

The committed read-side covers:

- connector health
- per-stream health
- in-memory resume markers (`lifenum`, last `replstate`)
- trading session state
- instrument dictionary
- instrument-to-matching mapping
- client limits
- client positions
- own active orders
- own trades

The current Phase 3E projection surface is intentionally private-state-first.

Projected stream families and tables:

- `FORTS_TRADE_REPL`
  - `orders_log`
  - `multileg_orders_log`
  - `user_deal`
  - `user_multileg_deal`
  - `heartbeat`
  - `sys_events`
- `FORTS_USERORDERBOOK_REPL`
  - `orders`
  - `multileg_orders`
  - `orders_currentday`
  - `multileg_orders_currentday`
  - `info`
  - `info_currentday`
- `FORTS_POS_REPL`
  - `position`
  - `info`
- `FORTS_PART_REPL`
  - `part`
  - `sys_events`
- `FORTS_REFDATA_REPL`
  - `session`
  - `fut_instruments`
  - `opt_sess_contents`
  - `multileg_dict`
  - `instr2matching_map`

Settlement-account tables and broader reference-data parity remain deferred.

## Transaction Staging Model

The projector keeps two explicit layers:

- committed maps and committed snapshot vectors exposed to readers
- staged transaction copies built lazily during `TN_BEGIN ... TN_COMMIT`

Row application rules:

1. `TN_BEGIN` activates an empty staged transaction.
2. `STREAM_DATA` updates staged domain copies only.
3. committed snapshots remain unchanged during row staging.
4. `TN_COMMIT` atomically swaps staged domain copies into committed state.
5. committed snapshot vectors are rebuilt only for domains touched by the transaction.

This keeps read-side visibility commit-bounded and deterministic without adding threads or copy-heavy full-state cloning on every event.

## Invalidation Rules

Invalidation is explicit and stream-owned.

- `P2REPL_LIFENUM`
  - synchronizes the latest lifecycle marker
  - invalidates committed state across all declared private streams
  - clears committed row watermarks and commit sequence markers
  - leaves stream entries addressable but no longer online/snapshot-complete
- `P2REPL_CLEARDELETED`
  - clears only the committed domains owned by the target stream
  - preserves unrelated domains
  - preserves the stream’s accumulated `clear_deleted_count`

Current stream ownership boundaries:

- `FORTS_TRADE_REPL`: trade-owned order source and own trades
- `FORTS_USERORDERBOOK_REPL`: user-orderbook and current-day order source
- `FORTS_POS_REPL`: positions
- `FORTS_PART_REPL`: limits
- `FORTS_REFDATA_REPL`: sessions, instruments, matching map

## Snapshot API Shape

`Plaza2PrivateStateProjector` exposes committed read-only snapshots only.

Available accessors:

- `connector_health()`
- `resume_markers()`
- `stream_health()`
- `sessions()`
- `instruments()`
- `matching_map()`
- `limits()`
- `positions()`
- `own_orders()`
- `own_trades()`

The API returns `const` references or `std::span<const ...>` views. Staged state is not exposed.

## Fake-Engine Integration Model

Phase 3E keeps the Phase 3D fake engine narrow. The only extension is a minimal callback surface so the projector can consume:

- lifecycle events
- row batches
- commit boundaries

Added callback hooks:

- `CommitListener::on_event(...)`
- `CommitListener::on_stream_row(...)`
- existing `on_transaction_commit(...)`

The fake engine still:

- has no networking
- has no live runtime dependency
- has no hidden framework behavior
- remains deterministic and single-threaded

## Tests Added

New Phase 3E scenarios:

- `tests/plaza2_cgate/scenarios/private_state_projection.yaml`
- `tests/plaza2_cgate/scenarios/private_state_clear_deleted.yaml`
- `tests/plaza2_cgate/scenarios/private_state_lifenum_invalidation.yaml`

New Phase 3E tests:

- `plaza2_private_state_projection_test`
  - session / instrument / matching / limit / position / order / trade projection
  - stream health and watermark projection
  - current-day plus trade-delta order merge
- `plaza2_private_state_visibility_test`
  - proves rows staged inside a transaction are not visible before `TN_COMMIT`
  - proves committed state becomes visible only at commit
- `plaza2_private_state_invalidation_test`
  - selective `P2REPL_CLEARDELETED` behavior
  - `P2REPL_LIFENUM` invalidation across committed private-state domains

Existing Phase 3D scenario parser/materializer/rule tests continue to validate deterministic replay inputs and illegal lifecycle ordering.

## Known Limitations

- No persistence is implemented yet; resume markers are in-memory only.
- Settlement-account limits and positions remain deferred.
- Reference-data projection is intentionally narrow and limited to the tables needed for the shipped snapshot surface.
- No live runtime wiring is added here; Phase 3E is validated primarily through the fake engine.
- `replAct` row deletion semantics are not interpreted yet beyond explicit `CLEARDELETED` and `LIFENUM` invalidation.

## Why This Fits a 2–4 vCPU Colocated VPS

The design stays compatible with the later lean colocated runtime target because it:

- is single-threaded
- uses numeric `StreamCode` / `TableCode` routing instead of string-heavy dispatch in the apply loop
- keeps mutation local to touched domains
- avoids background workers and hidden queues
- exposes compact committed snapshots rather than reflection-driven runtime objects
- pushes deterministic scenario parsing and schema validation offline into earlier phases

## Next Recommended Phase

Phase 3F: external test-gated bring-up.

That phase should reuse the committed private-state projector from Phase 3E without changing its transactional visibility model.
