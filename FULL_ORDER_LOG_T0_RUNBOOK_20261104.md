# Full Order Log T0 — Wednesday 4 November 2026 — UNARMED

All times are Europe/Moscow. This is a one-day internal TEST plan, not certification or authority to start. **Nothing runs tonight. Each block requires a separate owner start message; no timer, wake-up, cron, at or systemd schedule is installed.** PR68, the certified AGGR binaries, their configs and state remain untouched. Use only the reviewed PR70 FOL executable and its separate immutable release. No PROD or T1 fallback.

## 4 November hold after read-only failure

The original-profile full-link retry passed SOFTWARE_AUTH and committed REFDATA/matching ID1, but the composite repeatedly failed `invalid row transaction or size` before ONLINE. **B/C/D remain unstarted/on hold until a valid read-only book is demonstrated.** No order or read-only restart is implied by the old timetable. See [the diagnostic review](FULL_ORDER_LOG_DIAGNOSTIC_REVIEW_20261104.md). The diagnostic release retains strict transaction/size admission and needs its own separately authorized qualify-only run (<=5 minutes, explicit absolute TERM deadline, full vendor links). Existing298d9b1e deployment inputs are historical; future commands must select the new reviewed diagnostic manifest explicitly.

## Snapshot follow-up and deferred blocks

The [snapshot admission review](FULL_ORDER_LOG_SNAPSHOT_REVIEW_20261104.md) supersedes the diagnostic-only admission policy for the next pinned release. Its deployment does not authorize a native run. A separate owner message may authorize qualify only for <=10minutes, explicitly choosing the new BIN/SCHEMES, an absolute TERM deadline and original full vendor links. Report time to ONLINE, all snapshot-table counts, info position, online framing, book validity/lag and strikes/exit. Proposed afternoon window14:05–14:50; avoid14:55–15:10.

B is deferred to5November around08:00, conditional on a valid read-only book and the unchanged3-minute writer/session gate. A can run that morning. C/Kairos is deferred to5November10:20–11:20. Each still needs its own owner start message; D stays held. The4November timetable below is historical, not start authority. No timer or wake-up is installed.

## Schedule and block boundaries

MOEX's [current T0 schedule](https://www.moex.com/s438), checked3November, lists07:00–10:00 morning trading,10:00–10:19:45 opening auction,10:20–14:00 main,14:00–16:00 evening,15:00 settlement and16:00 mark-to-market; weekdays are ordinary T0 days. This is the current schedule endpoint; the older `/a550` page has different weekend wording. A clock time does not prove the system is up or a selected instrument is tradable. Recheck current notices and native session/instrument state on4November. Never start the ORDLOG router before the trading system starts; do not probe the exchange before07:00.

| Block | Planned window | Mode and action | Required result |
|---|---|---|---|
| A | Around07:15; bounded end specified in its start message | Read-only `plaza2 qualify`, selected ISIN and depth20; no publisher | Restricted-scheme OPEN accepted; REFDATA + composite snapshot then ONLINE; matching ID logged; committed book only |
| B | Around08:00; bounded end specified in its start message | FOL `plaza2 run`, one selected instrument; passive Day Add/Move/Del and scoped DelUserOrders. IOC is a separate subsection; no intentional fill without explicit owner authorization | Native replies plus authoritative private commits/terminal states; no blind retries, unresolved orders or residual position |
| C |10:20–11:20 | Read-only `qualify`; no orders/publisher |60-minute book/ORDBOOK comparison and live rates/lag/CPU/RSS; no persistent cross or reopen loop |
| D |14:50–15:30 | FOL writer, one passive Day Add/Del around15:00 settlement | Document order/book behavior across clearing and authoritative cancellation/flatness; no new entry after15:20 |

Every block is separately preflighted and stopped. At the block's explicit start, set its existing ownership-verified TERM-deadline safeguard to that block's approved end. There is an absolute **15:45MSK stop for FOL and then the ORDLOG router**. Begin D cleanup by15:30 and retain15minutes for recovery. No block is launched automatically because the preceding block passed. An unsuccessful block is preserved and reported, not silently retried under its old start message.

## Frozen deployment and preflight without connections

Use the exact commit/SHA reported in PR70 and the matching immutable `releases/<40-character-commit>/bin/MANIFEST.json`; do not follow a mutable latest alias. `$BIN` is the FOL executable, `$SCHEMES` its sibling public9.12 `scheme/` bundle. Check binary/library/scheme/config hashes and modes, CI, storage and current source identity before connecting. The two client INIs must match the reviewed manifest. They are published vendor projections, not a prior negotiated T0 capture; A supplies native acceptance evidence.

Reviewed code/release pin: `298d9b1e4e42ab02bf97d7d3ecf8f24d9b8fada8`; FOL binary SHA-256 `28605ce793a7178f85227364d49c561502097dac0b7e822900f5cd1a87a23c5d`, beneath `/home/azgaldov/moex/full-order-log-4102/releases/298d9b1e4e42ab02bf97d7d3ecf8f24d9b8fada8/bin/`. The following documentation-only update does not retag or replace that tested executable. The sibling read-only comparison probe is also pinned in that release manifest. `$RUNTIME` is the dedicated FOL root; `$CGATE_LIBRARY` is its `lib/libcgate.so` (SHA-256 `f63e726a8482b793c3af755a8dc2b9ebb5cd727d88fb58ebb3fe9704a155ce6f`). Actual T0 compatibility is checked in A.

Dedicated router profile: `/home/azgaldov/moex/full-order-log-4102/config/router.t0.ini`, own link profile, secret file and logs; loopback127.0.0.1:4102 only. Do not modify the existing4101 router. Authentication secret is the saved permanent T0 ORDLOG credential in `secret/router-auth-t0.ini`, mode0600; never echo it or include it in commands/reports. Load the software key privately. Keep vendor default client/router logging; do not enable tracing, debug verbosity or per-poll timing journals. The ordinary FOL minute metrics remain enabled.

Before every block, verify no trading process and no other account client/command producer is running, and identify the current router/process PIDs with start times. Read-only blocks use distinct `app_name`s; B/D/recovery use the one registered writer instance and its durable state. Require no working/queued/unknown orders and current flatness before new entry; do not infer either from a stopped process. Select ISIN from fresh committed REFDATA, current session membership/matching and native tradable instrument state, not yesterday's cached numeric ID.

The binding source is the restricted20October TEST ORDLOG USER/REFDATA/PART capture referenced by `MOEX_Claude_handoff_CURRENT.txt`, section “20 October: T0 permission, account mapping and readiness”. Re-read those native receipts: exactly matching USER/REFDATA rows establish the seven-character account and matching PART, client level3 and clear SMA disable flags. They map ORDLOG and AGGR to the **same account**. Load the ORDLOG broker/client values from the reverified restricted mapping, not AGGR state or the login spelling. This proves the historical binding, not current flatness or order acceptance.

**AGGR and FOL trading never run concurrently.** Instrument-wide `DelUserOrders` with `ext_id=0` covers both logins' orders on this account and instrument; disjoint ID ranges do not isolate it. Quantity, position, open-order and notional limits are account-level: include all existing account exposure and external terminal activity, not just this process's orders. The first single passive Day Add in B is the native order-entry-right test; any native rejection stops B, is reported and is not retried. No new password/entitlement theory is inferred from that rejection.

The separate FOL ID registration must be present, nonoverlapping with AGGR2000070000–2000119999 and all other historical assignments, and have sufficient unused startup/command capacity. Never reset/checkpoint-copy the AGGR namespace or reuse its instance. Registration is preparation, not execution authority.

Registration completed unarmed3November: `moex_full_order_log_20261104`, inclusive **2000120000–2000169999** (50,000 IDs), in `deployment/20261104/id-registrations.ndjson`. Receipt/ledger SHA-256 `9938a7fe8d2c8fc03888ba31633b7d8af4a18b4136a1f7fa7bd2c4a45dd9cbdf`; global discovery mirror is `qualification/nov04-ordlog-id-registration-a1`. All650 historical hashes, including64 checkpoints and the AGGR ledger, were unchanged; zero IDs consumed and no checkpoint created. Recheck actual remaining IDs before each future writer/recovery start. This is not authority to start or submit an order.

### Measured clock input

Immediately before starting each block capture `chronyc tracking`, `chronyc sources` and `timedatectl show -p NTPSynchronized`, with UTC receipt time. Require a selected synchronized source, `Leap status: Normal`, recent reference time and `NTPSynchronized=yes`. Convert **System time**, not Last offset, to signed integer microseconds: fast of NTP is positive, slow is negative; round the measured value. Pass that as `--clock-offset-us` for write/recovery and save raw tracking plus the conversion. Never enter an assumed zero or change the clock with `makestep` during a test.

Example observed3November17:11MSK: System time0.000000637seconds fast gives **+1us** after rounding; Last offset was a different diagnostic. This example expires before tomorrow's preflight. Record root delay/2 + root dispersion + absolute system residual as the reported NTP uncertainty; refuse an unsynchronized clock, absolute residual>1ms or this reported bound>10ms. The flag records the measurement; it does not set the host clock. The FOL gate tolerates at most1second of future exchange time and uses stream liveness, not the age of a quiet instrument's last row. Report live exchange→commit lag with the clock uncertainty, without claiming exchange-clock calibration from NTP alone.

## A and C: read-only

After that block's start message and confirmation the system has started, launch only the dedicated ORDLOG router. Run the FOL binary `plaza2 qualify --live` with the approved runtime paths, `--expected-release SPECTRA9.12.0`, `--instance-id` for that read-only block, `--trading-instance-id moex_t0_qualification_20261021`, router4102/trading-router4101, fresh ISIN, `--dtc-depth 20`, and loopback DTC port11300. `qualify` forces read-only; do not add `--allow-orders`. Keep DTC clients bounded and record/disconnect them at stop.

A records every actual OPEN table/field contract, matching ID, snapshot/ONLINE sequence and commit publication; any incompatible schema is a stop for order blocks. C additionally runs the reviewed raw `p2repl://FORTS_ORDBOOK_REPL;tables=orders,info` comparison probe on the ORDLOG router. Use its existing exact life/revision comparison rules and filtered composite schemes; unmatched/interior targets remain unassessed, not a guessed match. Log every LifeNum/ClearDeleted with available source/table/revision and investigate resting-order removal (P9). Report callback/row rates, exchange lag, maximum native process-loop time, CPU/RSS, crossings, missing-order counts, reconnects and0xA006. Zero0xA006 and no persistent crossed book are pass criteria, not assumed outcomes.

Stop with SIGTERM to the verified FOL/probe PIDs, then the ORDLOG router, within the block deadline; require zero clients/listeners/processes and preserve native default logs plus metrics. C cannot continue into D without a new message.

### Block C: additive read-only Kairos check

This check stays entirely inside C10:20–11:20 and its approved stop deadline. It does not merge Kairos, alter any DTC bind address or authorize another block. Use Kairos built from [kairos-main PR111](https://github.com/Ruhkukah/kairos-main/pull/111)'s verified head. On3November the PR is open/unmerged at `a5bbda87fdf5c1ca339507ac5923ee78eaa3b82d`, based on `codex/moex-live-validation-20260918`. Before C record the actual source commit, executable SHA-256 and current merge state; a PR head is not an executable digest. No Kairos binary was built or launched by this documentation update; its build/SHA remains to be recorded before the check, never assumed from another build.

Run on TEST or use an explicitly started SSH local-forward to the connector's existing loopback port, for example `ssh -N -o ExitOnForwardFailure=yes -L 11300:127.0.0.1:11300 <test-vps-ssh-alias>`. Define that alias only in the private `~/.ssh/config`; keep host, SSH port and user out of this public repository. Do not open the tunnel tonight. For multiple configured ISINs forward each consecutive configured port and use **one Kairos DTC client per ISIN port**. Record client/tunnel connect and disconnect times; close them within C before shutting down FOL/router. Keep the connector bound to127.0.0.1.

Record PASS/FAIL/UNOBSERVED independently for:

- Initial type145 snapshot and type700 FullOrderLog authority envelope received; subscription depth20 per side (show20 where available, explicitly record fewer available levels).
- Batches applied atomically using field8 first/middle/final markers, contiguous field14 sequence and matching field13 epoch. Record sequence gaps, reconnects and any rejected batch; a stable picture alone does not prove this contract.
- Kairos best bid/ask equals the connector's committed top at at least10 sampled UTC timestamps, with screenshots or Kairos logs and connector provenance sufficient to correlate the same commit/version. Unaligned samples remain unassessed, not guessed matches.
- Display withdraws if invalidity occurs; if no invalidity occurs in C, report this check UNOBSERVED. No fault, invalidity injection or SIGSTOP is added by this plan.
- No order-entry surface for this source. No Kairos orders/accounts action is tested.
- Display lag versus exchange_moment_ns if exposed, naming measurement clock/uncertainty; otherwise mark unavailable. Preserve gaps/reconnects and visual/protocol failures without declaring end-to-end success from connector-only tests.

## B and D: orders through FOL only

### Mandatory B opening: writer status only, no Add

A reports the session histogram **UNAVAILABLE in pinned qualify**. Qualify does not expose per-book session authority and its order-entry status is false by construction; the diagnostic probe prints counters only. Do not infer a histogram or writable readiness from A. The binding authority gate is moved to B's own writer, without another connection or code change. The historical20October histogram remains unavailable as documented in `FULL_ORDER_LOG_SESSION_AUTHORITY_OPEN_20261103.md`.

1. Only after B's separate start message, launch pinned298d9b1e `plaza2 run --live --allow-orders`, using the registered instance and original journal/state. Send **NO Add** and no Move/IOC/DelUserOrders during this observation. Immediately engage `kill on` through the same writer's command socket. No command producer may enqueue an Add before or during it.
2. Poll `plaza2 cmd ... "status"` every30seconds for at least3minutes (at least7 samples including0s and180s). Save each complete status response with UTC receipt time. Per candidate ISIN record `book.source_session_id`, `session_terms.sess_id`, `book.valid`, `book.crossed`, `book.best_bid`, `book.best_ask`, `book.exchange_moment_ns` and `order_entry_ready`. Missing/null values remain explicit; include refusal diagnostics and native state where available. Do not substitute status from another process or previous block.
3. Immediately before the first Add, require the **latest** sample to have `source_session_id > 0`, equality to `session_terms.sess_id`, valid=true, crossed=false and order_entry_ready=true. This source ID is `committed_session_id`, not a per-order histogram: it is nonzero only for a valid, nonempty book whose retained orders all share one positive session; mixed/contradictory authority gives0. Empty/invalid/unknown state must not be called mixed solely from zero. In pinned code, the readiness field does not include the operator kill switch, so status may be ready while kill remains on; leave it on throughout observation. Only after all predicates, account/risk checks and the latest sample pass, issue `kill off` immediately before the single first passive Day Add.
4. If source_session_id stays0 with a valid two-sided book on **all** samples, treat the candidate as mixed: `kill on`, SIGTERM writer, then SIGTERM ORDLOG router, verify zero clients and **zero orders sent**, preserve the samples and report. Missing, inconsistent, stale or otherwise failing predicates also prohibit Add; do not poll indefinitely past B's deadline or relax the gate. A persistently mixed candidate moves B to a later separately authorized block, after an authority proposal based on latest committed session/sys_events, still refusing zero/unknown/future sessions, has tests, review and a new pinned release. No such code change is authorized tonight.

Use the registered writer instance with original journal/identity path across restarts. Configure the same ORDLOG login for data and orders, verified broker/client environment variables, assigned range, measured clock, TEST and `--live --allow-orders`. For one instrument use quantity1 per command, configured quantity/position cap2, open-order cap5, rate5/s and an explicit quote-notional cap. No latency/profiling flags. `--sole-instance` is permitted only after proving this account/instrument is exclusively controlled; it is required for an actual instrument-scoped DelUserOrders test.

The concrete writer invocation is the [cleanup invocation](FULL_ORDER_LOG_CLEANUP_20261104.md), **without `--cancel-only`**, using the normal command socket. Require private/current snapshots, valid FOL book and stream liveness before each Add/Move. Day passive prices are recalculated from fresh exact-tick FOL and current limits; do not hard-code yesterday's prices or intentionally cross. Maintain unique client-order IDs and account for Move aliases.

```text
status
place <B-day-id> <ISIN> buy 1 <fresh-passive-price> day
move <B-day-id> 1 <fresh-passive-replacement>
cancel <B-day-id>
place <B-duo-id> <ISIN> buy 1 <fresh-passive-price> day
cancel-all <ISIN>
status
```

Each line is individually observed/confirmed, not an unattended batch. Socket acceptance is insufficient: record exact native request/reply plus subsequent relevant private state. Accepted186 blocks new entry until scoped terminal reconciliation; unrelated TRADE commits cannot release fallback or Add permission. No uncertain Add/Move is resent. Start D only after a new message and complete preflight; its one passive Day order is documented before/during/after15:00, then cancelled by15:30 (earlier if state/liveness becomes uncertain).

All resting orders are Day. IOC is prepared only in B, with a fresh nonmarketable price for the intended zero-fill check; it is not represented as guaranteed zero-fill because the market can move. A deliberate fill/marketable IOC requires explicit owner authorization. If that authority or a safe current IOC condition is absent, mark IOC/fill coverage deferred and do not claim it passed. Any unexpected fill stops further entry, preserves exact execution/POS evidence and is reported for the owner's explicit position disposition; never hide it behind an automatic compensating Add.

For an incompatible FOL OPEN with working orders, follow [the exact cancel-only cleanup procedure](FULL_ORDER_LOG_CLEANUP_20261104.md). Recovery uses the same FOL binary/login/state and no market-data listener; it never uses certified AGGR. Do not leave cleanup until15:45. Unresolved order/position state makes the block FAIL/incomplete, even if processes have stopped.

## End-of-day evidence

For each independently authorized block retain the start message, absolute TERM deadline, hashes, nonsecret effective profile/limits, NTP measurement, broker/client proof reference, registered-ID receipt, native/app logs and source-table/life/revision evidence. Order blocks add per-command reply/commit offsets, aliases, cancellations, executions/POS and flatness. Seal stopped evidence before rotation. Report unmeasured/pending items, including native rights/schema acceptance, raw comparison ambiguities and any missing IOC/fill coverage. FOL/R7 certification remains outside this internal day.

## ONLINE authority follow-up — 4 November

The f9f50463 read-only run authenticated and reached composite ONLINE, but remained invalid because it waited for an info callback which CGate consumed internally for stream binding. It is superseded for future qualification by the ONLINE-completeness correction described in [the snapshot review](FULL_ORDER_LOG_SNAPSHOT_REVIEW_20261104.md). Select only the newly pinned immutable BIN/SCHEMES after a separate start message; the old staged environment remains unsuitable. Validate ONLINE, book validity, row counts/framing, lag and shutdown on that run. Missing exposed info means exact raw comparison may remain UNALIGNED; report this limitation instead of claiming a match. All order blocks remain held pending valid native qualification, review and their separate start messages. No process, timer or router is started by this update.
