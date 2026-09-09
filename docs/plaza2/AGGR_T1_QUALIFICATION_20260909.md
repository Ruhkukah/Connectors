# Aggregated-mode T1 qualification, 9 September 2026

Base: PR #45 `25747b47138473d71213ed7019bfd0be0f4dd139`. Qualification branch is stacked on that head;
PRs #38–#45 remain unmerged. The user authorizes this one-day TEST exercise, including bounded Add/DelOrder
and a restart of the dedicated qualification router after a zero-order gate. Main login / Aggregated mode
is user-provided configuration evidence. Never open public ORDLOG, public ORDBOOK or p2ordbook.

## Same-day authorization amendment

At 09:42 MSK on 9 September the user explicitly moved qualification to today. This supersedes the original
10 September date. Preserve the prior preparation. Start after current preflight and validation; the pre-07:00
window cannot be recovered and must be NOT_OBSERVED. Baseline, clean restart and controlled router tests may
run later outside exchange transitions if their gates pass. Retain actual times. Do not repeat this run tomorrow.

## Pre-test audit and limits

The exact 96-row pre-test matrix is retained in
[the review directory](../review/plaza2_aggr_t1_qualification_20260909/matrix_pre_test.json).
MD03–MD05 now distinguish PR #42 offline recovery from absent T1 exercises. Shared AGGR invalidation and
one-second CLOSED/ERROR reopen exist. ClearDeleted uses full invalidation and fresh snapshot+online;
selective revision-range compaction is not implemented. LifeNum must await new committed data and ONLINE.

Private listeners deliberately fail after an established stream enters ERROR. Connection-level automatic
recovery is not implemented by this qualification tranche. A disruption may therefore demonstrate a
qualification failure. Preserve that result before any supervised application restart. Never describe a
manual restart as automatic recovery. Publisher-open/reply-open host flags are not actual current CGate
states; qualification instrumentation samples those actual states independently and requires all ACTIVE
before accepting an order request.

The live transport enforces zero starting position, quantity one, at most four ticks from the touch and
BBO age at most 5000 ms. Do not widen or bypass it. The runner uses the most passive four-tick boundary,
checks committed REFDATA price bounds, rejects expiry within 24 hours and retains exact canonical plans.
The supervisor must additionally verify liquidity and that four ticks is materially passive for the
selected instrument, recording the spread, distance and basis points. Otherwise mark the order scenario
SKIPPED_SAFETY. Do not substitute an expiring or illiquid contract to manufacture an order result.

## Runner and evidence

`plaza2_aggr_qualification OUTPUT SECONDS plaza2 qualify [existing moexctl configuration options]`
uses the existing persistent ConnectorHost. `OUTPUT` must be a new directory. The explicit date guard
allows CGate initialization only on 2026-09-09, 06:58–16:10 MSK. Set
`MOEX_AGGR_T1_AUTH=20260909_AGGREGATED_QUALIFICATION` and `MOEX_AGGR_T1_JOURNAL` to a dedicated persistent
journal directory. The separate `MOEX_AGGR_T1_ORDER_AUTH=20260909_ONE_LOT_ADD_CANCEL` enables the existing
TEST order purpose/arm; without it no order can be submitted. Preserve the same target and journal across
the clean restart. Host-managed ext_id starts at 2026090900; user IDs start at 2026090901/2/3 and advance
through the existing persisted serial/epoch mechanism. Validate no identifier collision before enabling.

The private deployment wrapper reads the existing protected account profile and secret environment;
never print or commit those values. The wrapper copies the client logging INI into each dedicated
vendor-log directory and changes only its logfile path, preserving the shared original. Its launch receipt
records that private INI hash and the package manifest hash. Pin source, binary,
runtime, scheme, config and router hashes in preflight. The existing 30 messages/second local cap counts post attempts. Qualification submits only one serial
Add/Cancel cycle per scheduled scenario, retaining capacity for cancellation; the local cap is not a
claim about the login's provisioned exchange limit.

The runner counts every callback and TN boundary, validates every committed AGGR state for valid side,
positive retained volume, exact decimal/scale agreement, unique side/price keys and depth <=20, and samples
book/state hashes, canonical per-instrument hashes and CPU counters once per second. Lifecycle/reply events use a bounded 8192-event buffer;
loss is explicit and blocks orders. Callback formatting and disk writes occur outside CGate callbacks.
The qualification-only validation scan adds work to commit; measure its actual Linux cost and do not
represent these timings as an uninstrumented connector benchmark. Preserve native vendor traces for
full reply diagnostics and schema-negotiation evidence. REPLSTATE text is copied into bounded lifecycle
storage and emitted as hex outside the callback; signed ClearDeleted boundaries, table codes and flags
are retained in the numeric record.

`events.log` columns are monotonic_ns, stream code, kind, value, error, message_id, user_id, signed_value, table, flags, text_hex. Event kinds
0–9 follow Plaza2ListenerEventKind; kind 13 records host readiness transitions and 14 records callback failure (including decoder failures); 10/11/12 are sampled connection/publisher/listener states. TN counters
are exact but per-TN timestamp traces are not collected. CGate states are CLOSED=0, ERROR=1, OPENING=2,
ACTIVE=3. Owner identity and cross-thread violations are recorded. Current RSS and FD count are sampled on Linux;
the supervisor additionally records filesystem headroom and preserves actual process-error diagnostics. maxrss_native_units is KiB on Linux,
bytes on macOS. `price_limits_current.json` rows are isin_id, session_id, replRev, lower, upper; they are
published only after REFDATA TN_COMMIT. Instrument expiry is Unix seconds from the runtime decoder.

After all scenario gates pass, write a new `order.request` atomically inside the live output directory:
`buy PRICE BASE_CONTRACT` or `sell PRICE BASE_CONTRACT`. The runner consumes the request before binding
and never retries Add. It writes the canonical plan, submits through `begin_order`/`submit_order`, observes
Working and sends one explicit cancel, then requires safe Cancelled before finishing the epoch. A
request is not permission to bypass supervisor checks. Any unexpected fill, ambiguous result, failed
cancel, stale readiness, observation failure or event loss ends new orders. Retain the journal and raise
operator attention; do not automatically flatten or resubmit. No automatic command files are scheduled. The qualification configuration disables the existing
DelUserOrders recovery path even if cancellation loses its correlated order identity; this fails closed
without allocating or posting another command. Defaults for other hosts are unchanged.

Set `MOEX_AGGR_T1_IDLE=1`, omit the order authorization and use duration exactly 300 for the independent
connection-only probe. It opens no listener or publisher and records polling/state/CPU evidence. Run it
in a second process/work directory alongside the main host.

## Timeline and supervision (MSK)

| Directory | Window | Required action |
|---|---|---|
| 00-preflight | 06:50–06:57 | Refresh MOEX notices; hashes, Aggregated declaration, NTP, >=20 GiB free, no stale connector, router dependencies, disabled ORDLOG automation |
| 01-startup | 06:58–07:10 | Start host before 07:00; measure committed AGGR bootstrap, ONLINE/readiness, >=10 instruments where available; select fresh non-expiring target from REFDATA/BBO |
| 02-baseline-order | 07:10–07:20 | One minimum-lot passive Add, Working, immediate DelOrder, Cancelled; independent own-order/position reconciliation |
| 03-app-restart | 07:30–07:45 | Zero-order gate, graceful app restart, same journal/identity, fresh private/AGGR bootstrap, one gated order; separate idle probe |
| 04-router-restart | 08:30–09:00 | Confirm dedicated router and zero orders, stop/start only that router; preserve automatic recovery result before intervention |
| 05-network-loss | outside exchange transitions | Optional 20–30 seconds T1-only isolation with independently verified automatic rollback and intact SSH; otherwise SKIPPED_SAFETY |
| 06-opening-main | 10:35–11:05 | No orders/disruption across 10:40 auction–10:59:45–11:00; structural AGGR checks; one order only after stable trading |
| 07-hot-reserve-1215 | 12:10–12:20 | Zero orders; observe actual MOEX switch; continuity is valid evidence, not an inferred outage |
| 08-post-reserve-order | 12:20–12:30 | One order only following fully reconciled healthy recovery |
| 09-evening-transition | 13:55–14:10 | Observe 14:00 settlement/expiry/evening; order only after stable valid session; observe 13:00 expiry without expiring orders |
| 10-clearing-1500 | 14:55–15:10 | Zero orders, observe clearing and generations; optional order only after verified trading resumes |
| 11-session-end-1600 | 15:55–16:10 | Zero orders by 15:55, 16:00 MTM, final reconciliation and observation through 16:10 |

MOEX sources checked 2026-09-09: [T1 availability](https://www.moex.com/s328),
[T1 schedule](https://www.moex.com/s438),
[12:15 hot-reserve notice dated 2026-08-26](https://www.moex.com/n103686?nt=107).
Availability must be refreshed immediately before the run. A notice is not evidence of an observed event.

Keep the principal process running across exchange events. A discovery host with a provisional positive
target/session can collect REFDATA and all AGGR instruments without orders; choosing the actual target
requires a clean replacement host because the existing host has no live retarget API. Record that as
startup configuration, never as successful mid-run recovery. If a failure prevents observation, preserve
it, reconcile zero orders, and only then use a new explicitly labeled observation process to avoid losing
later exchange-event evidence. Do not silently restart during the hot-reserve window.

Each scenario must retain environment.json, scenario.json, result.json, events.log, metrics.json,
state_before.json, state_after.json and hashes.json. Use new directories, immutable after finalization.
Supplementary process logs can be linked by hash and exact intervals rather than copied repeatedly.
Always distinguish cumulative from interval counters and preserve the pre-disruption state.

## Closeout

Statuses: PASS, FAIL, PARTIAL, NOT_OBSERVED, SKIPPED_SAFETY, BLOCKED_EXTERNAL. No manufactured LifeNum,
ClearDeleted, flood 99, timeout 100, TCS reload or retention reset. Unknown/unavailable evidence cannot PASS.
All 96 matrix rows require explicit scoped outcomes, source SHA, scenario/time/evidence references and
limitations. Full ORDLOG remains intentionally outside this run, retaining its backlog status.

After final independent reconciliation, write REPORT.md, validation.json, scenario_matrix.json,
day_timeline.json, environment.json and artifact_hashes.json under the review directory. The final verdict
is AGGR_MODE_READY_FOR_FREEZE, AGGR_MODE_REQUIRES_FIXES or AGGR_MODE_INCOMPLETE_EXTERNAL_SCENARIOS.
This preparation document is not an end-of-day report or certification verdict. Stop after closeout;
pause the one-day heartbeat. Do not merge PRs, change MOEX login configuration, force server rate errors,
restart the VPS, or touch unrelated trading services.

Regression preparation found a timing-dependent TWIME Establish-timeout fixture. It now waits for the peer
to receive Establish before advancing the fake clock and checks the exact 100/101 ms boundary. This changes
the test synchronization only, not TWIME production behavior; retain the initial failed run in validation.

## Live bootstrap defect retained

The first 9 September attempt on `2ca378fdf1dbac941e3e72f7a90144c8d49763f0` failed AGGR bootstrap:
ClearDeleted(MAX_REVISION) arrived before any snapshot transaction and the bridge repeatedly reopened.
Private streams synchronized, but AGGR had zero commits and never became ONLINE; no publisher posts occurred.
The supervisor stopped the process and sealed its evidence before a code fix. This attempt remains FAIL.
The fix treats cleanup of an already empty pre-snapshot projection as a no-op. Cleanup during a transaction
or after ONLINE still invalidates and requests a fresh snapshot. The fake-runtime regression sends the real
startup marker sequence on every open, including recovery opens; it fails before the fix and passes after.

The normal host order/position flatness assessment is target-scoped. Qualification additionally samples
all visible POS rows and active/conflicting own-order rows, without target filtering. Before any order or
router disruption require this account census to be zero and the underlying private streams to be healthy.
`private_exposure_current.json` retains sorted position and active-order evidence with account identifiers
hashed. Its hash describes current exposure, not the entire historical replication dataset. Metrics include
`account_active_orders`, `account_nonzero_positions` and `private_exposure_sha256`. Sampling is opt-in and
outside callbacks. A nonzero position blocks subsequent deliberate orders; no automatic flattening occurs.

The live retest also exposed a qualification-collector issue: cleanup markers for unrelated REFDATA tables
cleared already committed futures price limits. Only cleanup of fut_sess_contents now invalidates that
collector; unrelated table cleanup cannot erase its bounds. Regression covers both table cases. Order
scenarios remain blocked until the fresh committed bounds are actually retained and validated on T1.

The next live bootstrap showed that fut_sess_contents also repeats a retention boundary after its records
are sent. Clearing the entire bounds cache at that boundary was wrong: rows at or above the boundary remain
current. The qualification bounds collector now applies table/revision retention, staging in-transaction
cleanup until commit and allowing fresh revisions after a MAX reset. This is reference-data correctness;
the AGGR whole-book recovery policy remains unchanged. Limit-row counts separately distinguish missing
account data from a matching row whose limits_set flag is false. No account-limit trading guard is relaxed.
