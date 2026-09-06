# PLAZA II TEST trade transport V1.4 / PR #30 review

## 2026-09-06 AGGR20 replication-slot correction

The read-only T1 diagnosis found that a row can change price under the same
`replID` (observed row 664 moving between 12.950 and 12.931). The installed
MOEX 9.9 specification states that a price/volume/direction update replaces
the previous price level. Keying the projector by price and side retained
obsolete levels and could manufacture a crossed book.

The projector now replaces/deletes by `replID`; zero volume and nonzero
`replAct` remove that slot, and an instrument reassignment refreshes both
affected instrument snapshots. Transaction staging/rollback and the existing
reset paths remain intact. No lifecycle, authorization, publisher, or market
readiness gate is weakened.

Regression coverage includes the exact false-cross mechanism, side changes,
identity-based deletion with changed price/side, tombstones, multiple updates
to one slot, rollback, instrument reassignment, and reset. The multi-instrument
fake fixture also now changes the signed `replID` field, ensuring distinct
slots; its previous unsigned-field mutation had no effect on encoded IDs.

Changed-target ASan/UBSan: 5/5 passed. The first complete parallel CTest run
after fixture correction encountered the unrelated TWIME establish-timeout
test. The final unchanged-code sequential full CTest passed 158/158, including
the no-send, style, and ABI checks; `git diff --check` passed. Fresh live
qualification remains a separate gate.
Plan `70a099cc...` and its pending run are retired; they are not qualification
evidence for the corrected projector. Actual publisher posting remains
physically disabled.

## Scope and stop gate

This increment started from merged-main `610dbfd` (PR #29) and now includes
merged-main `cc2494d` (PR #32, including the CGate 9.9 prerequisite and typed
REFDATA row provenance). It keeps
the TEST-only boundary but adds the narrow `OfflineFake` and
`LiveTestPreSend` host modes. The latter may open a real TEST session and all
read-side/private services, but its post barrier is physically below publisher
message allocation and posting. There is still no production mode, broker
executable, Python/VPS wrapper, or live order command.

No order, `cg_pub_msgnew`, or `cg_pub_post` was used. Offline validation uses
the repository fake CGate runtime; any live pre-send observation is read-only
and requires the explicit TEST operator arms.

## One-session host

`Plaza2TestSessionHost` owns one `Plaza2Env`, one connection, the five private
replication listeners, the current-day `SESSIONSTATE` and `INSTRUMENTSTATE`
status listeners, the AGGR20 listener, one untyped `p2mqreply` listener, and
one publisher. `OfflineFake` requires the loopback connection and the private
`moex_fake_cgate_runtime_v1` marker. `LiveTestPreSend` validates the TEST
operator gate and may use a real runtime. Publisher creation/opening precedes
the matching reply listener, and both are bound to explicit settings with one
unique publisher name (`p2mq://...;name=N` and `p2mqreply://;ref=N`).
Shutdown closes and destroys publisher/listeners in reverse dependency order,
then the connection and environment. Private state, AGGR20 state, reply
correlation, and publisher operations share one connection and one event loop.

The private callback bridge is shared with `Plaza2LiveSessionRunner`. It
preserves per-stream LifeNum invalidation, table/revision/flags-scoped
CLEARDELETED, transaction visibility, USERORDERBOOK periodic consistency,
CLOSE invalidation, and the runtime row `replRev`.

`Plaza2TestTradeTransport` implements `OrderLifecycleTransport`. The lifecycle
controller binds one exact `PreSendPlan` after plan-hash validation and before
journal creation or transport startup. The concrete transport retains that
plan SHA and rejects an AddOrder unless the plan canonical JSON, AddOrder
payload hash, exact-ext recovery payload hash, profile, IDs, environment, and
policy all equal its authorized intent. It posts the already encoded AddOrder,
DelOrder, and exact-ext DelUserOrders payloads by
message name, preserves CGate's `DefinitelyNotSent`, `PossiblySent`, and
`Posted` certainty, never retries an AddOrder, and polls committed private
replication plus correlated replies. A publisher timeout remains uncertainty;
it is never converted into a rejection or cancellation.

For a live operator handoff, the same transport may instead start and warm its
single host before a plan exists. `install_authorized_intent()` then accepts one
candidate only after the host is started and the existing intent validation has
passed; a rejected candidate leaves the slot empty, while a successful install
is immutable. Constructor-supplied intents, duplicate/replacement installs,
and installs after plan binding are rejected. The method performs no bind,
preflight, receipt write, publisher allocation, or post, so plan generation,
authorization, binding, and the physical no-send barrier can all use one
continuously pumped host.

AddOrder is accepted only after a recomputed canonical authorized intent (with
payload, participant, identifier, profile, and fixed entry-policy fingerprints)
binds the encoded command byte-for-byte and the target-specific execution-safety receipt
has been atomically published. Loopback alone is not treated as evidence of a
fake runtime. The canonical fixed policy includes `max_aggr20_age_ms` and
`require_zero_starting_position`; transport configuration may only apply a
stricter age/position override. The receipt records the authorized maximum and
zero-position policy alongside observed local age and a typed position-evidence
class. A zero starting position is accepted only from an exact zero POS row or
from a complete POS snapshot plus an anchored, current TRADE replay with zero
participant deals and zero active own orders; a missing POS row by itself
remains unresolved.
The receipt is derived from the current committed projectors immediately before
the post and contains the target BBO, local monotonic age, exchange evidence,
session/instrument state, exact client limit-row hash, stream readiness,
publisher/reply-listener state, trading capability, passive-price and distance
checks, and quantity-one policy. It also freezes the three typed target
REFDATA records (`fut_instruments[isin_id]`, `fut_sess_contents[isin_id]`, and
`session[sess_id]`) with their table codes, typed identities, `replRev`, and
current REFDATA LifeNum. All three records must belong to the same current
LifeNum. The stream-wide `last_commit_sequence` is not accepted as target-row
provenance and may legitimately be zero. DelOrder and exact-ext recovery deliberately
skip this entry gate so cleanup remains available after market-data staleness.

The pre-send active-order census is independent of the proposed `ext_id`: any
positive-rest order for the same participant and target that is present in the
current USERORDERBOOK/current-day tables blocks the gate, even when its `ext_id`
belongs to an earlier run. When deferred `FORTS_TRADE_REPL` is opened, the host
persists the immutable `{trades_rev, trades_lifenum, server_time}` POS anchor it
used. Flat replay is accepted only while the current POS revision/lifenum still
match that anchor; either change fails closed and does not auto-reanchor.

MOEX TEST does not reconcile `FORTS_USERORDERBOOK_REPL` with
`FORTS_TRADE_REPL`. They are independent evidence surfaces: USERORDERBOOK is
used only for the current own-order snapshot in the pre-send active-order
census, while TRADE `orders_log` and `user_deal` are used for lifecycle
correlation and POS-anchored participant-trade replay. Projected order rows
therefore retain separate source identities even when the streams reuse an
`ext_id` or exchange order id. No TEST readiness, lifecycle consistency, or
position result may be made inconsistent merely because the two surfaces have
different identities, amounts/rest, states, row counts, or revisions.

The reply bridge accepts only active fixed user IDs, retains the raw payload,
decodes the locked message family with `Plaza2TradeCodec`, and reports timeout
with only the originating user ID. Unknown users, malformed payloads, wrong
families, and contradictory duplicate replies fail the poll while leaving the
cleanup/reconciliation path available.

## AGGR20 and freshness invariant

`Plaza2Aggr20BookProjector::snapshot_for_isin(isin_id)` is the only scoped BBO
surface intended for trading. The older `snapshot().top_bid/top_ask` fields are
retained as explicitly diagnostic cross-instrument values and must not be fed
into an order decision. Each scoped commit carries:

- local `steady_clock` commit time, captured inside the projector (tests inject
  the clock);
- exchange moment and moment-nanoseconds;
- instrument-specific depth, revision, and best levels.

There is no operator-supplied age field in this projector path. A transaction
updates scoped freshness only for affected `isin_id` values; unrelated
instrument traffic cannot refresh the target. A missing, one-sided,
crossed/locked, deleted, stale, non-tradable, or non-target instrument is not a
ready target and must
not authorize a post. Target readiness also requires current-day
`fut_sess_contents` membership, current session and instrument status rows, a
futures min-step, the exact session in state `1` (running; Add + Cancel
allowed), every required private stream and AGGR20 epoch online and
snapshot-complete, USERORDERBOOK `periodic_snapshot_consistent=true`, exactly
one matching seven-symbol client limit row with `limits_set=true`, and (when
requested) either exactly one matching client position row with the authorized
seven-symbol participant, expected `account_type` (`2` for a normal client,
locked BF semantics `1` for client code `000`), and `xpos=0`, or the accepted
POS-anchor/TRADE-replay evidence class described above. The receipt also binds
the POS anchor fields, replay completion and participant deal counts, the
reconstructed target `xpos`, and the active own-order census.

## Profile and ABI controls

The private TEST profile uses
`${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}` in `env_open_settings` and models
`MOEX_PLAZA2_CGATE_SOFTWARE_KEY` separately from the exchange credential
variable. The private runner rejects the exchange credential token in the
CGate software-key setting. No secret value is stored in Git.

The runtime adapter uses the reviewed MOEX CGate 9.3/9.9-compatible ABI:
`CG_MSG_DATA` and `CG_MSG_P2MQ_TIMEOUT` are decoded through the reviewed
`cg_msg_data_t` layout, including `owner_id`. The object-state ABI is
`CLOSED=0`, `ERROR=1`, `OPENING=2`, and `ACTIVE=3`; no ABI layout was changed.

## Offline semantic coverage

The single `plaza2_trade_transport_scenarios_test` executable exercises the
actual concrete transport and fake CGate runtime for:

- one coordinated Env/connection with private, AGGR20, p2mqreply, and publisher
  lifecycle plus reverse-order partial-start cleanup;
- accepted AddOrder, timeout uncertainty, exact-ext recovery, ordinary reply
  decode, timeout-without-fabricated-order evidence, malformed replies, wrong
  message families, contradictory duplicate replies, reply-before-replication,
  and cancellation-timeout uncertainty;
- full-fill and cancelled terminal observations, identity-conflict and
  contradictory AddOrder identity fail-closed results, and the Add-timeout
  recovery path to factual cancellation, including a working AddOrder followed
  by DelOrder and a publisher post timeout with recovery still available;
- exact plan binding before AddOrder, refusal without a binding, controller /
  transport policy-SHA mismatch before host startup, canonical age and
  zero-position policy continuity, correct normal-client account type, and
  wrong-account-type zero-position refusal;
- one-shot late authorized-intent installation after host start, including
  validation rollback, duplicate/replacement rejection, constructor-intent
  protection, post-bind closure, and the unchanged LiveTestPreSend no-send
  barrier;
- two instrument AGGR20 isolation and target-only freshness, one-sided/stale/
  crossed/locked/absent targets, scheduled/running/suspended/completed session states,
  missing or non-tradable session, missing or wrong client limit row, non-zero starting
  position, marketable and out-of-distance prices, unavailable
  p2mqreply/publisher, and receipt-persistence refusal;
- LiveTestPreSend's typed no-send barrier after a complete receipt, including
  publisher/reply identity validation and the POS-anchor flatness
  reconstruction fixture;
- truthful asynchronous listener readiness, a delayed initial REFDATA reopen,
  a same-anchor delayed TRADE reopen, and fail-closed TRADE recovery when the
  POS revision changes before retry;
- a same-participant active USERORDERBOOK order with a different `ext_id`,
  which must block pre-send; and unchanged, revision-drift, and LifeNum-drift
  POS-anchor fixtures, which retain the original anchor and fail closed on
  either drift;
- cleanup posting after the target AGGR20 has become stale.

The existing lifecycle scenario executable retains the transport-neutral V2
state-machine coverage, including sticky inconsistent-terminal semantics,
identifier-lock assertions, journal degradation, and restart reconciliation.

`reconcile_unfinished_run()` is a read-only startup mechanism: it never posts a
command, validates the immutable journal schema/run identity/profile/payload
and historical order IDs, retains locks for missing/working/conflicting or
mismatched observations, and writes a separate SHA-referencing prepared then
verified resolution record before releasing locks.

## Final invariant

There is exactly one human-authorized static intent from plan creation through
the concrete transport post. In the controller-started path, the controller
binds the canonical plan SHA before journal/host startup; in the live operator
handoff path, one started transport host installs one validated intent exactly
once before that same binding. The transport refuses AddOrder unless its
payload, recovery command, identifiers, profile, environment, and fixed entry
policy match the bound plan. Dynamic BBO, timestamps, session state, local age,
and position observations remain receipt evidence rather than authorization-
hash inputs. The deferred TRADE reconstruction is additionally bound to the exact
POS `{trades_rev, trades_lifenum, server_time}` anchor captured when that
listener open was requested. An accepted `cg_lsn_open` call is not replay
readiness: the selected anchor becomes ready only when the listener is ACTIVE
and TRADE has completed its snapshot and reached ONLINE. A later POS revision
or LifeNum is an unresolved mismatch, not a reason to re-anchor during the same
run. The active own-order census never uses the proposed `ext_id` as a filter.

Target identity is qualified from exact committed REFDATA rows, not from a
stream-wide activity counter. The `fut_instruments` row for the target, its
`fut_sess_contents` membership row, and the target `session` row must all be
present in the current REFDATA LifeNum, and the execution-safety receipt binds
each row's typed key, table code, `replRev`, and LifeNum. A missing row, a mixed
epoch, or a crossed/locked BBO remains a hard pre-send refusal.

The transport may release no identifier lock merely because a terminal-looking
row arrived. For any order that may have existed, a `Filled` or `Cancelled`
terminal is market-safe only when all correlated evidence is consistent. An
identity conflict, contradictory ordinary reply, or inconsistency carried
through exact-ext recovery remains `UnresolvedOrphanIncident`, with
`market_safe_terminal=false`, `evidence_consistent=false`, `ok=false`, and all
ext/user locks retained. Evidence consistency is sticky for the run; only
future restart-time reconciliation may clear the incident. DefinitelyNotSent
and a definitive correlated AddOrder rejection with no contradictory order
evidence may remain market-safe.

## Verification and stop

The final offline validation is recorded here after the bounded checks:

| Check | Result |
|---|---:|
| Full CTest | 158/158 passed |
| PLAZA label | 48/48 passed |
| TWIME label | 74/74 passed |
| Sanitizer label | 66/66 passed (ASan leak detection disabled for AppleClang) |
| .NET ABI checks | 2/2 passed |
| Changed-target ASan/UBSan | 5/5 passed (ASan leak detection disabled for AppleClang) |
| No-test/no-operator minimal build | passed |
| `git diff --check`, source/repo style, Unicode | passed |
| Native offline plan/privacy check | passed |
| MOEX/broker/network/live order activity | no order, `cg_pub_msgnew`, or `cg_pub_post`; publisher/reply open-only observation stopped `NOT_READY` |

The offline transport is ready for review. The bounded LiveTestPreSend gate,
if run after the offline checks, may open the TEST read-side services and
publisher/reply objects, persist one redacted receipt, and invoke only the
typed `SEND_DISABLED_PRE_SEND_PHASE` barrier. It must prove zero publisher
message allocations/posts and never authorizes a live order. PR #31 is now the
merged 9.9 prerequisite; any send-capable transport remains out of scope.

## Bounded T1 pre-send observation

The read-only `LiveTestPreSend` observation on 2026-08-28 at approximately
18:15 UTC used one generated publisher identity for both `p2mq` and
`p2mqreply`. The publisher reached ACTIVE and the matching reply listener
reached ACTIVE; no publisher message allocation or post was attempted. POS,
TRADE, USERORDERBOOK, PART, SESSIONSTATE, and INSTRUMENTSTATE reached the
observed states reported by the temporary operator probe, with
`USERORDERBOOK.periodic_snapshot_consistent=true` and no active own orders.

The gate was not order-ready in this external T1 window: REFDATA did not
reach snapshot-complete/ONLINE before the bounded stop, the candidate
`RIU6` mapped to session `11695` whose session and instrument public states
were both `0`, and no target two-sided AGGR20 BBO was available. Consequently
there is no execution-safety receipt and the typed post barrier was not
entered. This is an external `NOT_READY` result, not a relaxed repository
gate; a future order-ready window must satisfy every existing condition before
the disabled barrier can be exercised.

## CRU6 plan and transport-only follow-up (2026-08-29)

The successful read-only discovery observation selected CRU6 as `CNY-9.26`
(`isin_id=4433036`, `sess_id=11695`, `min_step=0.00100`) with session and
instrument public state `1`, a two-sided AGGR20 book, enabled PART limits, zero
POS rows, zero participant trades, and zero active own orders. The existing
builder/dry-run path produced the canonical quantity-one passive TEST plan
whose SHA-256 is
`82ecd8b0cdcaceb8b6eb5772392531fb04b6f84394a48bd95ca141113ecce6aa`;
the copied plan is
`evidence/pr30-live-pre-send/cru6_pre_send_plan_82ecd8b0.json`.

The first `LiveTestPreSend` control with that exact SHA (09:06 UTC) was run
before the temporary driver enforced its readiness stop. Its actual output
was `target_ready=0`, followed by the concrete cross-check refusal
`operator target tick size contradicts authoritative instrument min_step`;
it did not produce a receipt. The redacted log is
`evidence/pr30-live-pre-send/cru6_transport_only_20260829T090608Z.log`.

A follow-up diagnostic now prints the fresh-host evidence and stops before
bind/pre-send whenever `target_ready=0`. It observed target `sess_id=0`, no
current-session membership or instrument status, an empty authoritative
`min_step` (configured `0.00100` parses to `1000`, instrument parses as
invalid), REFDATA `online=0/snapshot_complete=0/rows=0`, and TRADE
`online=0/snapshot_complete=0`. POS was online and complete with the retained
anchor `{trades_rev=2317518659, trades_lifenum=66739,
server_time=1787966677}`, USERORDERBOOK was online/complete and periodically
consistent, and AGGR20 was two-sided and fresh. The driver stopped with
`TARGET_READY_STOP target_ready=0; no bind or pre-send call`; its redacted log
is `evidence/pr30-live-pre-send/cru6_tick_diagnostic_20260829T113843Z.log`.
This classifies the fresh-host result as `REFDATA_NOT_CURRENT`, not a parser
or connector tick mismatch. No `cg_pub_msgnew` or `cg_pub_post` occurred, the
T1 router remained the same process, and no successful live execution-safety
receipt is claimed by these runs.

## Async-open diagnosis and bounded initial recovery (2026-08-31)

The complete execution-host P2Log changes the preliminary diagnosis. The
listeners did not enter ERROR and were not stuck in OPENING. The connection
reached ACTIVE at log timestamp `13:35:17.801`; REFDATA changed from CLOSED to
OPENING at `13:35:17.833`, then to ACTIVE at `13:35:20.163`; and TRADE changed
from CLOSED to OPENING at `13:35:22.267`, then to ACTIVE at `13:35:24.416`.
The host began closing at approximately `13:35:25.35`, less than one second
after TRADE became ACTIVE and before REFDATA/TRADE delivered the ONLINE and
snapshot-complete evidence used by the projector. No ERROR transition was
recorded for either listener.

The actual defect was therefore premature readiness: immediately after the
deferred `cg_lsn_open` request returned OK, the host marked the anchored TRADE
replay ready. The temporary execution driver used that value in its warm-up
predicate, exited the observation loop, and stopped a healthy asynchronous
open before the snapshots completed. This was not a CRU6, TEST-stream
reconciliation, position-policy, or exchange-availability failure.

The host now preserves exact open settings and supervises the numeric CGate
state for the required private/status listeners. OPENING is allowed to
progress; an ERROR before the initial snapshot is closed and reopened no
sooner than one second later. This deliberately narrow recovery does not
attempt mid-run resynchronization: ERROR after a completed initial snapshot
fails closed. Deferred TRADE retains its originally selected POS revision and
LifeNum, retries only with the exact same settings while that anchor is still
current, and fails closed instead of re-anchoring if POS changes.

Deterministic fake-runtime regressions cover REFDATA initial ERROR followed by
delayed reopen and ONLINE, TRADE initial ERROR followed by same-anchor reopen
and ONLINE, and TRADE ERROR followed by POS-anchor drift. They also assert that
an accepted TRADE open request leaves `trade_replay_anchor_ready=false` until
ACTIVE plus snapshot-complete/ONLINE evidence exists. This correction neither
allocates nor posts a publisher message. A new live no-send receipt is still
required before PR #30's live merge gate is met.

After PR #31 merged, its new `main` was merged into this branch without
rewriting PR #30 history. The async-listener supervisor was corrected to the
reviewed CGate object states: `CLOSED=0`, `ERROR=1`, `OPENING=2`, and
`ACTIVE=3`. The existing deterministic REFDATA initial-open retry, same-anchor
TRADE retry, POS-anchor-drift refusal, and no-send scenarios pass against the
corrected fake runtime. No live run was performed for this correction.

All earlier CRU6 plans, including `82ecd8b0...` and `3dea722d...`, are obsolete
and must not be reused. A future 9.9 observation may create a new quantity-one
passive plan only after every observation gate is current, and must then stop
for explicit authorization of that exact new SHA.

The exact-head weekday retry at `6f15da99e4e1cbcfbc5089445c63b1f778af79e1`
then exercised the new supervisor against T1 at approximately 14:19-14:21
MSK. The connection, publisher, reply listener, and AGGR20 listener became
ACTIVE. Each of USERORDERBOOK, POS, PART, REFDATA, SESSIONSTATE, and
INSTRUMENTSTATE was opened between three and four times as CGate alternated
between OPENING/ACTIVE and `MQ:TIMEOUT` ERROR while the underlying services
were unavailable. This proves the bounded reopen path operated; it did not
make unavailable Exchange services current.

The final observation was therefore `OBSERVATION_READY=0`: REFDATA, POS,
TRADE, and USERORDERBOOK were not online/snapshot-complete; no immutable POS
anchor existed, so TRADE was never opened; and the last scoped CRU6 AGGR20 BBO
was stale. The driver stopped before bind or pre-send. The redacted log is
`evidence/pr30-live-pre-send/cru6_async_open_retry_6f15da9_20260831T1420MSK.log`
with SHA-256
`1d18eef096d1f136ee9e2483749c302420f902747bec032562b05664dad3dae9`.
The client operation log contained zero `cg_pub_msgnew` and zero `cg_pub_post`
calls, and router PID/start time was unchanged. This is
`T1_REQUIRED_REPLICATION_SERVICES_NOT_READY`, not a successful Gate A or a
reason to weaken readiness.
