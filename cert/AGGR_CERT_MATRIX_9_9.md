# SPECTRA 9.9 Aggregated-mode certification matrix

This is the current AGGR-only addendum to [the historical Plaza II matrix](PLAZA2_CERT_MATRIX.md). It is based on merged main `78f1dded089453d8e3d52a3f1fc26536baf1b197` and the locked SPECTRA 9.9 runtime in
[`aggr_plaza2_certification_manifest_9_9.json`](aggr_plaza2_certification_manifest_9_9.json). It does not relabel earlier evidence and it does not claim Full ORDLOG readiness.

`PASS_OFFLINE` means the software-controlled behavior is covered by deterministic tests or a reviewed lock. `PASS_OBSERVATION` is the bounded, order-free T1 observation recorded on 2026-09-14.
`NOT_RUN_T1_SESSION_CLOSED` means the row still needs a live run when the test market is open. `MOEX_COORDINATED` requires a scheduled exchange exercise.
`DEFERRED_FULL_ORDLOG_PHASE` is outside this candidate. A command family is `N/A_UNDECLARED` only when the candidate explicitly does not claim it.

| Row | Scope | Classification | Current result | Evidence or next gate |
|---|---|---|---|---|
| C1 / P2-C01 | connection and environment ownership | AGGR_REQUIRED | PASS_OFFLINE | `Plaza2TestSessionHost` single owner; TEST endpoint and CGate 9.9 lock |
| C2 / P2-C02 | private/status/AGGR listener set | AGGR_REQUIRED | PASS_OFFLINE | exact five private streams, two status streams, `FORTS_AGGR20_REPL` |
| C3 / P2-C03 | publisher and p2mqreply ownership | AGGR_REQUIRED | PASS_OFFLINE | same owner, unique publisher name, reply `ref` identity |
| C4 / P2-C04 | initial dependency order and fresh anchors | AGGR_REQUIRED | PASS_OFFLINE | POS.info anchor precedes TRADE; fresh AGGR snapshot is required |
| R1 / P2-R01 | snapshot and ONLINE/readiness | AGGR_REQUIRED | PASS_OBSERVATION | all effective readiness fields true in the 2026-09-14 observation |
| R2 / P2-R02 | LifeNum/generation invalidation | AGGR_REQUIRED | PASS_OFFLINE | private and AGGR invalidation fixtures; live transition remains below |
| R3 / P2-R03 | ClearDeleted handling | AGGR_REQUIRED | PASS_OFFLINE | full invalidation plus fresh snapshot/ONLINE fixture |
| R4 / P2-R04 | router/transport loss with no order | AGGR_REQUIRED | PASS_OFFLINE | bounded rebootstrap tests; live zero-order result is retained historical evidence |
| R5 / P2-R05 | listener/publisher/reply loss | AGGR_REQUIRED | PASS_OFFLINE | state-machine and no-resend tests |
| R6 / P2-R06 | retry/deadline/fatal classification | AGGR_REQUIRED | PASS_OFFLINE | one-second minimum retry, bounded deadline, causal errors preserved |
| R7 / P2-R07 | public ORDLOG/L3 mutation | DEFERRED_FULL_ORDLOG_PHASE | DEFERRED_FULL_ORDLOG_PHASE | no public ORDLOG/ORDBOOK/p2ordbook claim in AGGR candidate |
| R8a / P2-R08A | raw boundary and AGGR fallback | AGGR_REQUIRED | PASS_OFFLINE | AGGR fresh-snapshot fallback; public C2 remains deferred |
| R8b / P2-R08B | LifeNum boundary and generation | AGGR_REQUIRED | PASS_OFFLINE | generation markers and stale-state rejection |
| S1 / P2-S01 | AddOrder | AGGR_REQUIRED | NOT_RUN_T1_SESSION_CLOSED | one-lot deep-passive Add only after review and current gates |
| S2 / P2-S02 | accepted reply and exact Working evidence | AGGR_REQUIRED | NOT_RUN_T1_SESSION_CLOSED | must correlate reply 99 and private TRADE/USERORDERBOOK rows |
| S3 / P2-S03 | immediate DelOrder | AGGR_REQUIRED | NOT_RUN_T1_SESSION_CLOSED | one explicit Cancel only; no compensating command |
| S4 / P2-S04 | Cancel reply and exact Cancelled evidence | AGGR_REQUIRED | NOT_RUN_T1_SESSION_CLOSED | reply 100 plus both private surfaces and zero orders |
| S5 / P2-S05 | reply timeout ambiguity | AGGR_REQUIRED | PASS_OFFLINE | ambiguous result blocks new Add; no retransmit |
| S6 / P2-S06 | publisher cap and reply penalty | AGGR_REQUIRED | PASS_OFFLINE | 30/s default local cap, 1..3000 bounded configuration |
| S7 / P2-S07 | Move/MassCancel command claim | N/A_UNDECLARED | N/A_UNDECLARED | not declared by this candidate; no evidence implied |
| G1 / P2-G01 | clock synchronization and timestamps | AGGR_REQUIRED | NOT_RUN_T1_SESSION_CLOSED | capture NTP/monotonic/exchange clocks in the next full-day run |
| G2 / P2-G02 | process remains alive across session transitions | AGGR_REQUIRED | NOT_RUN_T1_SESSION_CLOSED | observe the complete published T1 day |
| G3 / P2-G03 | app restart with zero orders | AGGR_REQUIRED | NOT_RUN_T1_SESSION_CLOSED | controlled restart after zero-order gate |
| G4 / P2-G04 | app restart with Working order | AGGR_REQUIRED | NOT_RUN_T1_SESSION_CLOSED | restart reconciliation; no automatic Add or flatten |
| G5 / P2-G05 | router/TCS restart with Working order | MOEX_COORDINATED | MOEX_COORDINATED | schedule with MOEX; preserve uncertainty and reconcile |
| G6 / P2-G06 | schema modification | MOEX_COORDINATED | MOEX_COORDINATED | controlled MOEX exercise; incompatible drift must fail closed |
| G7 / P2-G07 | backup/access-server switch | MOEX_COORDINATED | MOEX_COORDINATED | controlled MOEX exercise; retain endpoint/config fingerprints |
| MD1 / P2-MD01 | AGGR initial synchronization | AGGR_REQUIRED | PASS_OFFLINE + PASS_OBSERVATION | exact rows, depth, decimal scale, snapshot/ONLINE |
| MD2 / P2-MD02 | AGGR online operation and freshness | AGGR_REQUIRED | PASS_OFFLINE + PASS_OBSERVATION | fresh BBO age 1ms in bounded observation |
| MD3 / P2-MD03 | AGGR recovery | AGGR_REQUIRED | PASS_OFFLINE; live NOT_RUN_T1_SESSION_CLOSED | controlled router/transport loss is next-session gate |
| MD4 / P2-MD04 | AGGR LifeNum | AGGR_REQUIRED | PASS_OFFLINE | stale generation invalidates effective readiness |
| MD5 / P2-MD05 | AGGR ClearDeleted | AGGR_REQUIRED | PASS_OFFLINE | full invalidation and new snapshot required |
| P9.9 | scheme compatibility/removals | AGGR_REQUIRED | PASS_OFFLINE | zero fatal drift; reviewed removals explicit; no active AGGR dependency |
| P9.9+ | additive fields | AGGR_REQUIRED | PASS_OFFLINE | `prevorder_id`, `premium_blocked` ignored safely as additive fields |

## Current gate

The matrix is intentionally **not ready** for the final certification freeze. The test market is closed today, so no Add, Working, Cancel, restart, or full-day claim is being manufactured.
The next live session must use a fresh current-session discovery and a new candidate binary/hash package; this matrix does not authorize an order by itself.
