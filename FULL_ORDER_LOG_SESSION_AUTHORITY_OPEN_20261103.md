# Long-lived resting orders: authority remains open

Claude accepted documentation head0d455002 over tested code298d9b1e. The pinned FOL binary remains SHA-256 `28605ce793a7178f85227364d49c561502097dac0b7e822900f5cd1a87a23c5d`. No source, binary, deployment, router or timer changes are made for this follow-up.

The current authority gate rejects mixed retained sessions. Deleting old orders can restore authority, but this does not establish whether legitimate long-lived orders with older sess_id should block a current book. This semantic question is unresolved.

## Offline20October evidence

Read-only inspection covered retained `qualification/oct20*ordlog*` directories on TEST. No public `FORTS_ORDBOOK_REPL` capture was found there. The successful `oct20-t0-ordlog-permanent-recheck-a2` reader source has SHA-256 `cc3575e2352fe50752f6baa361f5dd51d2f26e415a1cab182b8bdd71b0008169`; its line151 logs DATA metadata only when `g_row_count == 1`. Its ORDLOG vendor log is1,930,166bytes, SHA-256 `1cc4e43ccaab47546c62ec82402aaf6372c55afdf205b1e7709f0e980cf7ce14`. Counters/metadata do not preserve the full order rows and lifecycle needed to reconstruct resting state. Earlier sample/truncated logs do not provide the missing snapshot anchor.

Result: **historical resting-order session histogram unavailable**, not empty and not proof of a mixed or single-session book. The20October private USERORDERBOOK snapshot is account-only and cannot substitute for public ORDBOOK. No T0 connection or native process was started. Machine-readable inventory/hashes: `/private/tmp/moex-fol-oct20-histogram-audit-20261103.json`.

## Block A acceptance report

For each candidate ISIN, use A's own complete committed snapshot+ONLINE cutover to report:

| Field | Required evidence |
|---|---|
| Resting-order histogram | Counts by native sess_id, totals, retained/admitted versus excluded rows |
| Current session | Committed session_state sess_id at the same observation |
| Counterfactual FOL source gate | Would full_order_log_entry_ready pass with the writer's provider configured; every refusal reason and unavailable input |
| Provenance | Life/revision, snapshot/ONLINE/commit boundary and observation timestamps |

No extra connection or diagnostic instrumentation is authorized by this report. The pinned qualify executable's ordinary top-depth/metrics output alone does not expose a complete per-order histogram; capture sufficiency must be checked explicitly. If complete same-process evidence is unavailable, report incomplete and keep B blocked. Do not quietly add a listener or change the binary to obtain it.

A remains unarmed until the owner's separate start message after confirmed system start. Any mixed candidate blocks B. Report it and propose a latest-committed-session/sys_events authority rule that still rejects zero/unknown/future sessions, with tests and a newly pinned release; do not implement it tonight.
