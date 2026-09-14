# SPECTRA 9.9 Aggregated-mode certification matrix

This matrix is an AGGR-only mapping of the numbered Plaza II controls in Appendix 1 of the pinned MOEX
Customer Software Certification Procedure. `C01-C08`, `R01-R08`, and `S01-S07` are internal stable
traceability IDs mapped one-to-one to those controls; MOEX does not name the controls with these repository IDs.
They are not renamed or reused for different requirements. General requirements are listed separately. Full
ORDLOG/L3 remains `DEFERRED_FULL_ORDLOG_PHASE`.

Authority pins: procedure URL `https://www.moex.com/files/4qg0gqtzcxkep68687ah1bwq5e`, title
`CUSTOMER SOFTWARE CERTIFICATION PROCEDURE`, approved `2021-09-23`, retrieved `2026-09-14`, downloaded
SHA-256 `5602136d9b7865a648c255f1e284aa2b2b301089a6a30033baba987f4287cf42`, Appendix 1 Plaza II plus
general sections 1-2. General logging/connection context also uses the pinned VPTS document at
`https://www.moex.com/files/41j6qhzzp4hkdznn2wp8sj24ds`, title `Moscow Exchange Technical Center Software
and Hardware Suite Connection Requirements for Customer Software`, document date `2016-04-01`, retrieved
`2026-09-14`, downloaded SHA-256 `63e2018123dc2b5dcdacedb02eec99d915abbc75f5e2957e642efa133bbff510`.

Each official row has its own classification, offline result, T1 result, code/test evidence, exact T1 evidence,
and remaining action. `PASS_OFFLINE` means deterministic or locked software evidence. `PASS_T1` means exact
current-candidate T1 evidence. `NOT_RUN_T1_SESSION_CLOSED` records the closed session. `MOEX_COORDINATED`
requires an exchange-controlled exercise.

## Connection requirements

### C01 — connection URLs
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: `plaza2_runtime.cpp`; `plaza2_runtime_adapter_test.cpp`
- Exact T1 evidence: none on current candidate; capture URL receipt
- Remaining action: refresh notice and record authenticated TEST URL

### C02 — connection/thread ownership
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: `connector_host`; `plaza2_live_session_runner_test.cpp`
- Exact T1 evidence: none on current candidate; capture owner/thread map
- Remaining action: retain one-owner mapping in launch evidence

### C03 — >=300-second idle polling without losing router connection
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: runtime timeout tests; current runbook idle harness
- Exact T1 evidence: no 300-second receipt yet
- Remaining action: run 300-second no-listener/no-publisher probe

### C04 — connection to authenticated router
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: `plaza2_runtime_probe_test.cpp`; runtime lock
- Exact T1 evidence: no current-candidate authenticated receipt
- Remaining action: capture router/authenticated connection states

### C05 — router available while Plaza network is unavailable; wait/no-send
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: fake transport no-send and readiness tests
- Exact T1 evidence: no current-candidate targeted upstream-fault receipt
- Remaining action: attempt the safe client-controlled equivalent: keep local P2MQRouter running, block only its
  outbound T1 Plaza destination traffic, prove router reachable but upstream unavailable, wait/no-send/no-current-stream;
  if safety cannot be demonstrated, classify only that live portion MOEX_COORDINATED with the recorded reason

### C06 — detect Plaza network becoming available
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: bounded rebootstrap tests; `connector_host_test.cpp`
- Exact T1 evidence: no current-candidate targeted upstream-transition receipt
- Remaining action: remove the C05 outbound block on the same connector, prove upstream availability detection,
  fresh bootstrap and readiness without application restart; fall back to MOEX_COORDINATED only if the safe
  mechanism is unavailable

### C07 — detect Plaza network loss and stop sends/stream use until recovered
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: fail-closed transport/recovery tests
- Exact T1 evidence: no current-candidate targeted upstream-loss receipt
- Remaining action: with zero active orders and known position, reapply the C05 block, prove immediate
  effective-readiness/command loss and no stale-stream use, then remove it and prove bounded fresh
  rebootstrap; defer Working-order loss until zero-order recovery passes

### C08 — detect router connection loss and stop activity
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: `connector_host_test.cpp`; AGGR runner recovery test
- Exact T1 evidence: prior historical evidence is not relabeled
- Remaining action: run local-router loss/restart on T1; no Add retry

## Replication requirements

### R01 — subscription URLs
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: `plaza2_live_session_runner.cpp`; stream fixtures
- Exact T1 evidence: none on current candidate; record every URL
- Remaining action: capture all five private, two status, and AGGR URLs

### R02 — subscription/thread ownership
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: live runner ownership assertions; `connector_host_test.cpp`
- Exact T1 evidence: none on current candidate; record callback owner
- Remaining action: preserve one-owner listener map

### R03 — correct client receive scheme where applicable
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: explicit `p2repl://...;scheme=|FILE|...forts_scheme.ini|...` bindings; runtime scheme lock; scheme drift tests
- Exact T1 evidence: no current negotiated-scheme receipt
- Remaining action: capture negotiated stream schemes and hashes

### R04 — compatible server-scheme additions
- Classification: N/A_CLIENT_SCHEME
- Offline result: PASS_OFFLINE
- T1 result: N/A_CLIENT_SCHEME
- Code/test evidence: compatibility checker and reviewed 9.9 additions retained as defense-in-depth/version validation
- Exact T1 evidence: not applicable to this explicit client-scheme profile
- Remaining action: keep machinery green; do not represent a server-scheme exercise as mandatory for this profile

### R05 — incompatible removal/type-change detection
- Classification: N/A_CLIENT_SCHEME
- Offline result: PASS_OFFLINE
- T1 result: N/A_CLIENT_SCHEME
- Code/test evidence: runtime drift tests; 9.9 removal guard retained as defense-in-depth/version validation
- Exact T1 evidence: not applicable to this explicit client-scheme profile
- Remaining action: keep fail-closed drift machinery; no mandatory server-scheme MOEX exercise for this profile

### R06 — loss and correct reopening of every declared stream
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: `connector_host_test.cpp`; AGGR runner; live-stream fixtures
- Exact T1 evidence: no per-stream current-candidate receipt
- Remaining action: exercise POS, PART, TRADE, USERORDERBOOK, REFDATA, both status, and AGGR

### R07 — >=100k msg/s Full ORDERS_LOG
- Classification: DEFERRED_FULL_ORDLOG_PHASE
- Offline result: DEFERRED_FULL_ORDLOG_PHASE
- T1 result: DEFERRED_FULL_ORDLOG_PHASE
- Code/test evidence: public ORDLOG/L3 excluded from AGGR host
- Exact T1 evidence: no AGGR claim
- Remaining action: qualify after T1 account switch in separate tranche

### R08 — ClearDeleted and LifeNum
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: private/AGGR invalidation and generation tests
- Exact T1 evidence: no current LifeNum/ClearDeleted transition receipt
- Remaining action: capture committed boundaries and fresh-generation readiness

## Sending requirements

### S01 — publisher URLs
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: transport configuration and publisher tests
- Exact T1 evidence: none on current candidate; record publisher/reply URLs
- Remaining action: capture exact publisher and reply bindings

### S02 — publisher/thread ownership
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: single-owner host tests; `connector_host_test.cpp`
- Exact T1 evidence: none on current candidate; record owner/thread
- Remaining action: retain same-owner evidence during lifecycle

### S03 — configurable rate control
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: publisher rate tests; bounded 1..3000 guard
- Exact T1 evidence: no provisioned-cap receipt
- Remaining action: record configured exchange cap without flood

### S04 — correct send scheme
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: trade codec/spec-lock tests
- Exact T1 evidence: no current-candidate send receipt
- Remaining action: capture scheme/hash before any order

### S05 — replies and timeouts handled without undefined state
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: transport scenarios; lifecycle/recovery tests
- Exact T1 evidence: no current-candidate timeout receipt
- Remaining action: run ordinary lifecycle and preserve causal replies

### S06 — reply types 99 and 100 handled correctly
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: reply bridge and system-reply tests
- Exact T1 evidence: no deliberate flood/ambiguous live event
- Remaining action: decode any occurrence; never promote 99/100 to success

### S07 — publisher-loss detection and correct reopening
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: `connector_host_test.cpp`; publisher/reply recovery cases
- Exact T1 evidence: no current-candidate publisher-loss receipt
- Remaining action: reopen publisher/reply without retransmitting Add

## General requirements

### General — complete interaction logs
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: qualification journal/event-buffer tests
- Exact T1 evidence: no full-day indexed log on current candidate
- Remaining action: retain complete logs and hash index

### General — network interruption recovery
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: bounded recovery and no-resend tests
- Exact T1 evidence: no current full-day outage receipt
- Remaining action: run safe client equivalent and reconcile

### General — application restart during the trading day
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: restart checkpoint/reconciliation tests
- Exact T1 evidence: no current-candidate restart receipt
- Remaining action: zero-order restart, then one-Working-order restart

### General — TCS restart with reload
- Classification: MOEX_COORDINATED
- Offline result: PASS_OFFLINE
- T1 result: MOEX_COORDINATED
- Code/test evidence: generation/reload handling tests
- Exact T1 evidence: no exchange-controlled receipt
- Remaining action: schedule with MOEX

### General — TCS restart without reload
- Classification: MOEX_COORDINATED
- Offline result: PASS_OFFLINE
- T1 result: MOEX_COORDINATED
- Code/test evidence: retained-history/reopen tests
- Exact T1 evidence: no exchange-controlled receipt
- Remaining action: schedule with MOEX

### General — reserve/access-server switching
- Classification: MOEX_COORDINATED
- Offline result: PASS_OFFLINE
- T1 result: MOEX_COORDINATED
- Code/test evidence: endpoint/configuration validation
- Exact T1 evidence: no second-server receipt
- Remaining action: obtain MOEX-provisioned alternate and evidence

### General — full SPECTRA trading-day exercise with all declared command types
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_CLOSED
- Code/test evidence: runbook and lifecycle/reconciliation tests
- Exact T1 evidence: prior observation was order-free only
- Remaining action: run through published transitions and 12:15 hot reserve

## Current gate

The matrix is not certification-ready. The session is closed, so no new T1 evidence is manufactured. The next open session
must use fresh notices, hashes, current session discovery, a 300-second idle probe, and the principal full-day campaign.
The ordinary lifecycle is exactly
`AddOrder 474 -> {business reply 179 + matching private Working} -> DelOrder 461 -> {business reply 177 + matching private Cancelled + zero active orders} -> final position reconciliation`.
Each brace group is a conjunction; either channel may arrive first. System replies 99/100 are decoded and reconciled as
system events, never treated as ordinary success.
