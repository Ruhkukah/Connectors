# Long-lived resting orders: authority remains open

Claude accepted documentation head0d455002 over tested code298d9b1e. The pinned FOL binary remains SHA-256 `28605ce793a7178f85227364d49c561502097dac0b7e822900f5cd1a87a23c5d`. No source, binary, deployment, router or timer changes are made for this follow-up.

The current authority gate rejects mixed retained sessions. Deleting old orders can restore authority, but this does not establish whether legitimate long-lived orders with older sess_id should block a current book. This semantic question is unresolved.

## Offline20October evidence

Read-only inspection covered retained `qualification/oct20*ordlog*` directories on TEST. No public `FORTS_ORDBOOK_REPL` capture was found there. The successful `oct20-t0-ordlog-permanent-recheck-a2` reader source has SHA-256 `cc3575e2352fe50752f6baa361f5dd51d2f26e415a1cab182b8bdd71b0008169`; its line151 logs DATA metadata only when `g_row_count == 1`. Its ORDLOG vendor log is1,930,166bytes, SHA-256 `1cc4e43ccaab47546c62ec82402aaf6372c55afdf205b1e7709f0e980cf7ce14`. Counters/metadata do not preserve the full order rows and lifecycle needed to reconstruct resting state. Earlier sample/truncated logs do not provide the missing snapshot anchor.

Result: **historical resting-order session histogram unavailable**, not empty and not proof of a mixed or single-session book. The20October private USERORDERBOOK snapshot is account-only and cannot substitute for public ORDBOOK. No T0 connection or native process was started. Machine-readable inventory/hashes: `/private/tmp/moex-fol-oct20-histogram-audit-20261103.json`.

## Revised binding gate: B's own writer

A's histogram is UNAVAILABLE in pinned qualify; no per-book session authority or meaningful writable readiness is exposed there. Do not attempt to derive it from the counters-only probe or add a connection. The earlier proposed A histogram gate is superseded.

Only after B's separate start message, start its pinned writer on the registered instance/original journal, send no Add, engage kill on, and record writer command-socket status every30s for at least3minutes. For each candidate save source_session_id, session_terms.sess_id, valid/crossed, best bid/ask, exchange_moment_ns and order_entry_ready with UTC receipt time. On the latest sample all must satisfy positive/equal session IDs, valid, uncrossed and ready before kill off and the first passive Add. Pinned readiness does not include kill-switch state, so observation can keep kill on.

Source ID0 throughout with a valid two-sided book is treated as mixed: kill on, SIGTERM writer then router, zero orders sent, report and defer B. Empty/invalid/unknown states are distinguished and cannot grant entry. The semantic authority fix still requires review, tests and a new pinned release; no code change tonight. See the runbook for the exact sequence and C's additive read-only Kairos checks. Every block remains separately unarmed.
