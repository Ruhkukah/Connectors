# Phase 4A: PLAZA II + TWIME Reconciler

## Scope

Phase 4A adds an offline-first reconciler core for the approved `plaza2_repl + twime_trade` path. The new surface normalizes TWIME order/trade events, normalizes committed PLAZA private-state snapshots, and produces read-only reconciled order/trade snapshots plus reconciliation health.

## Non-goals

- no live dual-runner orchestration
- no new transport or network code
- no public market data
- no persistence or journaling
- no C ABI work
- no `plaza2_trade`

## Source-priority model

- TWIME is the fast intent and transactional-event path.
- PLAZA II is the replicated confirmation path and broader private-state view.
- TWIME may create provisional state before PLAZA confirms it.
- PLAZA may confirm, complete, or contradict that state.
- Contradictions are preserved explicitly as divergence or ambiguity. The reconciler does not silently overwrite disagreement.

## Normalized input model

### TWIME

`TwimeOrderInput`, `TwimeTradeInput`, and `TwimeSourceHealthInput` are exposed in [plaza2_twime_reconciler.hpp](/Users/pavel/CSharp/MoexConnector/connectors/plaza2_twime_reconciler/include/moex/plaza2_twime_reconciler/plaza2_twime_reconciler.hpp). Phase 4A adds helpers that normalize:

- outbound `TwimeEncodeRequest` application intents
- inbound `TwimeJournalEntry` application responses/reports
- session health and metrics snapshots

The reconciler never reparses SBE bytes itself. Byte decoding stays in the adapter helpers.

### PLAZA

`PlazaCommittedSnapshotInput` and `PlazaSourceHealthInput` normalize the committed Phase 3E projector surface:

- committed own orders
- committed own trades
- connector and stream health
- lifenum and replstate markers
- required-private-stream readiness
- explicit invalidation reason

`make_plaza_committed_snapshot(...)` is the narrow adapter from the Phase 3E projector.

## Matching hierarchy

The matching logic is conservative and deterministic:

1. Direct identifier match.
   Orders: TWIME `OrderID` to PLAZA `private_order_id`.
   Trades: TWIME `TrdMatchID` to PLAZA `id_deal`.
2. Exact fallback tuple.
   Orders: session, instrument, side, decimal-5 price, order quantity, and exact account/client code.
   Trades: session, instrument, side, exact TWIME order id mapped to the matching PLAZA order side, decimal-5 price, and exact fill quantity.
3. No fuzzy fallback.
   The reconciler never guesses by approximate price, quantity, or time.

If multiple fallback candidates exist, the entry becomes `Ambiguous`.

## Ambiguity and divergence rules

- `Ambiguous` means multiple deterministic candidates existed and no unique match was safe.
- `Diverged` means a direct-id or exact fallback match existed but materially disagreed on price, quantity, session, instrument, side, or order linkage.
- Both source views are preserved in the reconciled snapshot along with the explicit fault reason.

## Staleness model

Staleness is controlled by an injected logical step counter inside `Plaza2TwimeReconciler`. No wall-clock calls are used. `advance_steps(...)` deterministically ages provisional TWIME-only orders into `Stale`.

## Invalidation and resync handling

The reconciler consumes Phase 3E/3F invalidation state through `PlazaSourceHealthInput`:

- lifenum invalidation
- required-private-stream readiness loss
- connector online loss

When PLAZA invalidates, previously confirmed/matched entries are not deleted. They remain visible with `plaza_revalidation_required=true`, and health exposes the invalidation reason and pending revalidation counters.

## Snapshot surface

The committed read-only output surface is:

- `std::span<const ReconciledOrderSnapshot> orders()`
- `std::span<const ReconciledTradeSnapshot> trades()`
- `const Plaza2TwimeReconcilerHealthSnapshot& health()`

The snapshots preserve both TWIME-side and PLAZA-side views, reconciliation status, match mode, last update source, logical sequence/step, and explicit fault reason where needed.

## Tests added

- `plaza2_twime_reconciler_normalization_test`
- `plaza2_twime_reconciler_orders_test`
- `plaza2_twime_reconciler_trades_test`
- `plaza2_twime_reconciler_invalidation_test`

These cover provisional confirmation, reject handling, cancel/fill terminal states, divergence, ambiguity, deterministic staleness, PLAZA invalidation, normalized input adapters, and health counters.

## Known limitations

- Replace flows are conservative: the adapter preserves `prev_order_id` and replacement intent/acceptance, but full order-version lineage is still intentionally narrow.
- Trade staleness is not modeled as a separate status in Phase 4A; TWIME-only trades remain explicit via `TwimeOnly`.
- Phase 4A remains offline-first and does not orchestrate combined live TWIME + PLAZA sessions.

## Why this stays lean for 2–4 vCPU VPS use

The reconciler is single-threaded, explicit, and adapter-driven. Matching uses exact numeric and scalar fields already normalized by existing protocol layers, not reflection-heavy registries or fuzzy heuristics. The core keeps state in compact vectors/maps and performs recomputation synchronously on deterministic inputs, which is compatible with the low-overhead colocated runtime target.

## Next recommended phase

Phase 4B: expose the committed PLAZA + TWIME read side through the existing explicit C ABI style without adding default live order placement.
