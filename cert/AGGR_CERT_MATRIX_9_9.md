# SPECTRA 9.9 Aggregated-mode certification matrix

This matrix is an AGGR-only mapping of the numbered Plaza II controls in Appendix 1 of the current MOEX
VPTS certification procedure. `C01-C08`, `R01-R08`, and `S01-S07` are internal stable
traceability IDs mapped one-to-one to those controls; MOEX does not name the controls with these repository IDs.
They are not renamed or reused for different requirements. General requirements are listed separately. Full
ORDLOG/L3 remains `DEFERRED_FULL_ORDLOG_PHASE`.

## 2026-09-19 status-label refresh

No current-candidate T1 run was performed in this offline refresh. The earlier
`SESSION_CLOSED` labels were stale, so every affected row now uses
`NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED`. This means the current session status
was not reconfirmed here; it does not assert that T1 is open or closed and does
not promote missing live evidence.

## 2026-09-15 T1 evidence freeze and recovery correction

All 2026-09-15 T1 evidence remains immutable. The historical C03 receipt is
`PASS_T1 @ c4a0e392e0ed05eec7e264e3fc3cced6def22730`; it is not promoted to
the post-correction candidate, so C03 must be rerun on the next candidate.
The 10:26 r2 negative is retained as
`PARTIAL_SERVICE_AVAILABILITY -> WAIT/NOT_READY -> ZERO_POSTS` and is useful
historical evidence, but its eventual bounded `Recovering -> Failed` behavior
was not certification-compliant. The later read-only service experiment
resolved both unsuffixed `FORTS_TRADE_REPL` and `FORTS_TRADE_REPL_MATCH1`, plus
all three status streams, without a router/config/firewall change; the earlier
failure is therefore recorded as historical
`TRANSIENT_EXTERNAL_T1_SERVICE_AVAILABILITY`, not as a connector legacy-name
defect. Generic matching-aware replication remains a forward-compatibility
backlog item and is not implemented by this correction.

Authority pins: current procedure URL `https://www.moex.com/files/4xgv6e2x1paqr1zkn2fmq093cj`, title
`ПОРЯДОК СЕРТИФИКАЦИИ ВНЕШНИХ ПРОГРАММНО-ТЕХНИЧЕСКИХ СРЕДСТВ (ВПТС) ПАО МОСКОВСКАЯ БИРЖА`, approved
`2023-01-30` by order `МБ-П-2023-207`, retrieved `2026-09-14`, downloaded SHA-256
`91c24dd5d03947e03f4d2b9fa3a78ab1b4b9d5c8fc43dc91e9e7205135caf2f1`, Appendix 1 Plaza II plus general
requirements on pp. 2-4. The current VPTS requirements are pinned at
`https://www.moex.com/files/41w8g1tt63pd9tq9drmk4n3g4z`, title
`Требования к подключению клиентского программного обеспечения к программно-техническим комплексам Московской Биржи`,
current edition effective `2020-08-17`, retrieved `2026-09-14`, downloaded SHA-256
`a775f5c5aca3cefba58498549d8ff076055091faea757a6a0fbf1ba848f4a4a2`. The current 2023 Appendix 1 groups
remain equivalent to this repository's existing mapping: Connection 1-8 -> C01-C08, Replication 1-8 -> R01-R08,
Sending 1-7 -> S01-S07; no implementation remap is required.

Each official row has its own classification, offline result, T1 result, code/test evidence, exact T1 evidence,
and remaining action. `PASS_OFFLINE` means deterministic or locked software evidence. `PASS_T1` means exact
current-candidate T1 evidence. `NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED` records
that no T1 run was made and the current session status was not reconfirmed.
`MOEX_COORDINATED` requires an exchange-controlled exercise.

## Connection requirements

### C01 — connection URLs
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: `plaza2_runtime.cpp`; `plaza2_runtime_adapter_test.cpp`
- Exact T1 evidence: none on current candidate; capture URL receipt
- Remaining action: refresh notice and record authenticated TEST URL

### C02 — connection/thread ownership
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: `connector_host`; `plaza2_live_session_runner_test.cpp`
- Exact T1 evidence: none on current candidate; capture owner/thread map
- Remaining action: retain one-owner mapping in launch evidence

### C03 — >=300-second idle polling without losing router connection
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: runtime timeout tests; current runbook idle harness
- Exact T1 evidence: historical `PASS_T1 @ c4a0e392e0ed05eec7e264e3fc3cced6def22730`; not valid for the post-correction candidate
- Remaining action: rerun the 300-second no-listener/no-publisher probe on the next candidate

### C04 — connection to authenticated router
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: `plaza2_runtime_probe_test.cpp`; runtime lock
- Exact T1 evidence: no current-candidate authenticated receipt
- Remaining action: capture router/authenticated connection states

### C05 — router available while Plaza network is unavailable; wait/no-send
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: `connector_host_test.cpp` fake-clock tests A-C; indefinite operator-cancellable wait, alert-only threshold, readiness masking and zero-post guard
- Exact T1 evidence: no current-candidate targeted upstream-fault receipt
- Remaining action: attempt the safe client-controlled equivalent: keep local P2MQRouter running, block only its
  outbound T1 Plaza destination traffic, prove router reachable but upstream unavailable, wait/no-send/no-current-stream;
  if safety cannot be demonstrated, classify only that live portion MOEX_COORDINATED with the recorded reason

### C06 — detect Plaza network becoming available
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: `connector_host_test.cpp` fake-clock tests A-C; controlled retry, fresh transport generation/bootstrap and readiness restoration without application restart
- Exact T1 evidence: no current-candidate targeted upstream-transition receipt
- Remaining action: remove the C05 outbound block on the same connector, prove upstream availability detection,
  fresh bootstrap and readiness without application restart; fall back to MOEX_COORDINATED only if the safe
  mechanism is unavailable

### C07 — detect Plaza network loss and stop sends/stream use until recovered
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: fail-closed transport/recovery tests; fake-clock tests C-H cover no resend, no automatic cancel/flatten and unresolved-epoch retention
- Exact T1 evidence: no current-candidate targeted upstream-loss receipt
- Remaining action: with zero active orders and known position, reapply the C05 block, prove immediate
  effective-readiness/command loss and no stale-stream use, then remove it and prove bounded fresh
  rebootstrap; defer Working-order loss until zero-order recovery passes

### C08 — detect router connection loss and stop activity
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: `connector_host_test.cpp` fake-clock router wait/recovery and operator-stop tests; AGGR runner recovery test
- Exact T1 evidence: prior historical evidence is not relabeled
- Remaining action: run local-router loss/restart on T1; no Add retry; retain the old r2 fail-closed receipt without relabeling

## Replication requirements

### R01 — subscription URLs
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: `plaza2_live_session_runner.cpp`; stream fixtures
- Exact T1 evidence: none on current candidate; record every URL
- Remaining action: capture all five private, two status, and AGGR URLs

### R02 — subscription/thread ownership
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: live runner ownership assertions; `connector_host_test.cpp`
- Exact T1 evidence: none on current candidate; record callback owner
- Remaining action: preserve one-owner listener map

### R03 — correct client receive scheme where applicable
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
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
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: `connector_host_test.cpp` fake-clock per-listener/service recovery, fresh POS-to-TRADE bootstrap and status-stream gates; AGGR runner; live-stream fixtures
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
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: private/AGGR invalidation and generation tests
- Exact T1 evidence: no current LifeNum/ClearDeleted transition receipt
- Remaining action: capture committed boundaries and fresh-generation readiness

## Sending requirements

### S01 — publisher URLs
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: transport configuration and publisher tests
- Exact T1 evidence: none on current candidate; record publisher/reply URLs
- Remaining action: capture exact publisher and reply bindings

### S02 — publisher/thread ownership
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: single-owner host tests; `connector_host_test.cpp`
- Exact T1 evidence: none on current candidate; record owner/thread
- Remaining action: retain same-owner evidence during lifecycle

### S03 — configurable rate control
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: publisher rate tests; bounded 1..3000 guard
- Exact T1 evidence: no provisioned-cap receipt
- Remaining action: record configured exchange cap without flood

### S04 — correct send scheme
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: trade codec/spec-lock tests
- Exact T1 evidence: no current-candidate send receipt
- Remaining action: capture scheme/hash before any order

### S05 — replies and timeouts handled without undefined state
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: transport scenarios; lifecycle/recovery tests
- Exact T1 evidence: no current-candidate timeout receipt
- Remaining action: run ordinary lifecycle and preserve causal replies

### S06 — reply types 99 and 100 handled correctly
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: reply bridge and system-reply tests
- Exact T1 evidence: no deliberate flood/ambiguous live event
- Remaining action: decode any occurrence; never promote 99/100 to success

### S07 — publisher-loss detection and correct reopening
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: `connector_host_test.cpp` fake-clock publisher/reply recovery cases; no-post/no-resend guards
- Exact T1 evidence: no current-candidate publisher-loss receipt
- Remaining action: reopen publisher/reply without retransmitting Add

## General requirements

### General — complete interaction logs
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: qualification journal/event-buffer tests
- Exact T1 evidence: no full-day indexed log on current candidate
- Remaining action: retain complete logs and hash index

### General — network interruption recovery
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: indefinite operator-cancellable recovery, bounded retry/backoff, alert-only threshold, no-resend and unresolved-order-epoch tests in `connector_host_test.cpp`
- Exact T1 evidence: no current full-day outage receipt
- Remaining action: run the safe client equivalent and reconcile; the old r2 bounded-deadline behavior remains historical evidence only

### General — application restart during the trading day
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
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
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: runbook and lifecycle/reconciliation tests
- Exact T1 evidence: prior observation was order-free only
- Remaining action: run through published transitions and 12:15 hot reserve

### General — administrator/emergency procedure
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: `docs/plaza2/AGGR_OPERATOR_EMERGENCY_PROCEDURE_9_9.md`
- Exact T1 evidence: operator acknowledgment pending for next open session
- Remaining action: use the procedure for any ambiguity, order/position surprise, router/app/TCS failure

### General — per-instance customer-software identifier
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: nonempty `p2tcp` `app_name`, publisher/reply identity checks, instance-token test
- Exact T1 evidence: record the live `app_name` in qualification evidence
- Remaining action: prove the captured identity for the full session

### General — log/system-time ±1 sec
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: `plaza2_clock_evidence_passes` requires sync source/status, offset, monotonic ID and paired timestamps
- Exact T1 evidence: none while the session is closed
- Remaining action: record local wall, monotonic, exchange/server timestamps before the full-day run; any failure blocks PASS

### General — Exchange/NCC messages
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: `FORTS_REFDATA_REPL.sys_messages` projector, qualification snapshot and `moexctl qualify` JSON
- Exact T1 evidence: no current-session message receipt
- Remaining action: retain and hash the committed message evidence during the next open session

### General — administration/monitoring for broker systems
- Classification: N/A_PRODUCT_SCOPE
- Offline result: PASS_OFFLINE
- T1 result: N/A_PRODUCT_SCOPE
- Code/test evidence: product-scope decision in manifest and runbook
- Exact T1 evidence: not applicable
- Remaining action: none; this is proprietary connector software using a broker account, not a broker platform offered to clients

### General — one-to-one MOEX terminology
- Classification: AGGR_REQUIRED
- Offline result: PASS_OFFLINE
- T1 result: NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED
- Code/test evidence: current procedure/VPTS terms are used without local renaming of required concepts
- Exact T1 evidence: terminology review is part of next candidate package
- Remaining action: retain the one-to-one wording in operator evidence and certification correspondence

### General — fixed SPECTRA subsystem routing
- Classification: N/A_FIXED_SPECTRA_PROFILE
- Offline result: PASS_OFFLINE
- T1 result: N/A_FIXED_SPECTRA_PROFILE
- Code/test evidence: fixed FORTS/SPECTRA profile has no uncontrolled subsystem selector
- Exact T1 evidence: not applicable to this fixed profile
- Remaining action: do not add an operator-selectable subsystem without a new review

### General — broker-system/client-operation applicability
- Classification: N/A_PRODUCT_SCOPE
- Offline result: PASS_OFFLINE
- T1 result: N/A_PRODUCT_SCOPE
- Code/test evidence: proprietary connector runs through a broker account and is not client-facing brokerage software
- Exact T1 evidence: not applicable
- Remaining action: preserve this explicit scope decision

## Current gate

The matrix is not certification-ready. No new T1 evidence was manufactured
during the status-label refresh. Once a valid session and accepted candidate are
confirmed, a T1 run must use fresh notices, hashes, current session discovery,
a 300-second idle probe, and the principal full-day campaign. Do not infer an
open session or its date from this matrix.
The ordinary lifecycle is exactly
`AddOrder 474 -> {business reply 179 + matching private Working} -> DelOrder 461 -> {business reply 177 + matching private Cancelled + zero active orders} -> final position reconciliation`.
Each brace group is a conjunction; either channel may arrive first. System replies 99/100 are decoded and reconciled as
system events, never treated as ordinary success.
