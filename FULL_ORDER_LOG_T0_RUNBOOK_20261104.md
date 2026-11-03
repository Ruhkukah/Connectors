# Full Order Log T0 — Wednesday 4 November 2026 — UNARMED

All times are Europe/Moscow. This is a one-day internal TEST plan, not certification or authority to start. **Nothing runs tonight. Each block requires a separate owner start message; no timer, wake-up, cron, at or systemd schedule is installed.** PR68, the certified AGGR binaries, their configs and state remain untouched. Use only the reviewed PR70 FOL executable and its separate immutable release. No PROD or T1 fallback.

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

## B and D: orders through FOL only

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
