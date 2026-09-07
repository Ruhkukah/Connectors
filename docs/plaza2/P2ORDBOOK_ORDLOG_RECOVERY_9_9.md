# CGate 9.9 public order-book contract

Scope: Phase B, no L3 implementation or T1 qualification. The anonymous pair is
`p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;name=...`.
USERORDERBOOK/TRADE is a different, private pair.

Sources downloaded with verified HTTPS on 2026-09-07:

- [CGate API manual](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/cgate_en.pdf), dated 31.07.2026:
  receiving data, pp. 11-12; data schemes, pp. 13-16; listener creation/opening, pp. 32-36; tools, p. 51.
- [SPECTRA 9.9 manual](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/p2gate_en.html):
  anonymous snapshot stream, recovery algorithm, current-day data, replication service fields.
- Official SDK `cgate_linux_amd64-9.9.1853.zip`, header and `samples/src/samples/c/orderbook/ordbook.cpp`.
- Official `Scheme/9.9/ordLog_trades.ini` and `ordbook.ini`.

All source hashes, all 10 tables, all 135 fields, exact indices/offsets/sizes and TLS provenance are recorded in
`spec-lock/test/plaza2/public99/wire.json`. The SDK archive and library match the previously qualified distribution hashes.
The newly downloaded scheme is DDS 990.1.6.42752, not the old recorded 42744; all public field sequences match reviewed metadata.
No full-project runtime fingerprint is silently promoted by this public schema qualification.

## Answers to the twelve recovery questions

1. **Snapshot tables.** Default `snapshot.data=orders`; `snapshot.scheme` must include `orders` and `info`.
   The public snapshot scheme additionally defines `multileg_orders`, `orders_currentday`, `multileg_orders_currentday`,
   and `info_currentday`. Defining all six tables does not mean the default logical order stream projects all six.
   Snapshot and online records retain their respective layouts in a composite scheme; discover composite indices dynamically.
2. **Boundary.** The current API manual explicitly defaults `snapshot.bind` to **`info.trades_rev`**.
   The snapshot's `info` also contains `trades_lifenum` and `publication_state`; incomplete publications are not consistent.
   The old `info_currentday.logRev` prose example is stale. Actual current-day schema uses `trades_rev` too.
3. **Race handling.** CGate owns the snapshot-to-online binding in `p2ordbook`; use this facility, not an application race buffer.
   Documentation guarantees the bound transition, but does not specify whether internal buffering, remote retained history,
   or both implement it, nor buffer capacity. Those implementation details are **not claimed verified**.
4. **Snapshot completion.** `CG_MSG_P2REPL_ONLINE` marks completion of initial replication and transition to online data.
   `TN_COMMIT` provides table consistency, not completion of the entire initial snapshot. For a snapshot-only `p2repl`,
   `CG_MSG_CLOSE` may carry `CG_REASON_SNAPSHOT_DONE`; do not treat every CLOSE as successful bootstrap.
5. **Online completion.** The documented transition is `CG_MSG_P2REPL_ONLINE`; there is no second documented catch-up-complete
   event. Application Ready must additionally require its own mandatory work queue drained and committed valid state.
6. **Replstate.** The API's `cg_lsn_open` section says settings apply to `p2repl` only, not other listener types.
   Therefore do not pass raw ORDLOG `replstate` into `p2ordbook` as a verified optimization. Recreate the composite listener
   and bootstrap anew. A raw `p2repl` listener may reopen from the exact opaque token delivered before closure.
7. **LifeNum.** A change invalidates prior stream data and triggers retransmission. SDK orderbook sample clears its state
   on LifeNum. Invalidate the whole public generation; do not compare unrelated private/REFDATA life numbers to this pair.
8. **Unavailable snapshot.** Opening is asynchronous: return OK is not evidence of ACTIVE. Observe listener state and errors,
   close/reset on error and retry with bounded delay (manual suggests one second). No valid snapshot means no L3 readiness.
   Exact composite substream retry internals are undocumented; rely on observable errors, not an assumed fallback.
9. **Interrupted online stream.** Invalidate readiness and borrowed generations, then reopen under bounded supervision.
   The SDK sample clears its book/revision on CLOSE. Raw `p2repl` continuation is separate from rebuilding a composite book.
10. **Late join/restart.** Create the public `p2ordbook` listener, open with empty settings, validate its negotiated composite
    scheme after OPEN, process snapshot and online record kinds, wait for ONLINE and local committed catch-up.
    Restart without verified persisted book state means fresh composite bootstrap. No manual R+1 subscription is needed.
11. **ClearDeleted.** It identifies table index, revision and flags; rows in that table older than the boundary are deleted.
    `CG_MAX_REVISON` permits revisions to restart at 1. SDK sample maps composite order and order-log indices separately.
    It does not justify clearing every instrument on any ClearDeleted. Raw C1 retains the control event and resets its revision
    tracker only for the affected table at the maximum marker. L3 interpretation remains C2 work.
12. **Multileg.** Schemas define both multileg table families. `online.data`/`snapshot.data` select table names; the SPECTRA manual
    says multileg current-day tables are handled analogously. A dedicated pair using `multileg_orders_log` / `multileg_orders`
    is the candidate. Cross-pair atomic publication/ordering and actual composite callback indices need an offline vendor
    simulation or a later guarded T1 capture before **full multi-table L3** is claimed. The old SDK sample is single-table;
    it is not proof of multi-pair synchronization. C1 raw ORDLOG includes both tables without this dependency.

## Checkpoints and failure

The manual states REPLSTATE contains revisions/life/schema as of the last `TN_COMMIT`; events after it are retransmitted.
A callback error closes the listener, with REPLSTATE then CLOSE. This is a CGate delivery boundary, not proof that an application
consumer durably processed the transaction. Our raw delivery must hold checkpoint eligibility until all mandatory output is
acknowledged; a failed callback, incomplete transaction or unacknowledged output must prevent a later token being persisted.
The current private `ResumeMarkersSnapshot` is in-memory only; the existing order journal is not a raw/L3 checkpoint store.

Revisions are table-local update numbers. Numeric jumps alone do not prove transport loss, especially during history/snapshot
or filtered replication. Distinguish observed discontinuity from proven loss, legal replay, identical duplicate and corruption.
Never publish a later checkpoint after proven mandatory loss.

## Remaining C2 gates

Wire schemas, default public URL and default boundary are now authoritative. Still require:

- composite scheme/index callback fixtures for both table pairings and actual multileg publication coordination;
- deterministic LifeNum/close/reopen/retention-failure paths using the composite facility;
- public identity/rollover and full mutation invariants;
- complete hot-path performance including L3. Raw C1 performance is provisional.

No C2 implementation is authorized in this tranche.
