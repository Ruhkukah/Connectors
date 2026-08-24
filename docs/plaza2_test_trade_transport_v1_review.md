# PLAZA II TEST trade transport V1 review

## Scope and stop gate

This increment starts from merged-main `cc43c16` (PR #26). It adds a
TEST-only, fake-runtime transport path for offline semantic tests. It does not
add a live operator runner, a broker executable, a Python/VPS wrapper, a
production transport, or a network command. The transport host refuses a
non-TEST runtime and is exercised only with the repository fake CGate runtime
and loopback fixture.

No MOEX endpoint, broker account, CGate production runtime, TEST credential,
software key, `cg_pub_post` against a real runtime, or TEST order was used.

## One-session host

`Plaza2TestSessionHost` owns one `Plaza2Env`, one connection, the five private
replication listeners, the current-day `SESSIONSTATE` and `INSTRUMENTSTATE`
status listeners, the AGGR20 listener, one untyped `p2mqreply` listener, and
one publisher. Startup probes the runtime and requires the private
`moex_fake_cgate_runtime_v1` marker before opening the environment;
shutdown closes and destroys publisher/listeners in reverse dependency order,
then the connection and environment. Private state, AGGR20 state, reply
correlation, and publisher operations therefore share one fake connection and
one event-processing loop.

`Plaza2TestTradeTransport` implements `OrderLifecycleTransport`. It posts the
already encoded AddOrder, DelOrder, and exact-ext DelUserOrders payloads by
message name, preserves CGate's `DefinitelyNotSent`, `PossiblySent`, and
`Posted` certainty, never retries an AddOrder, and polls committed private
replication plus correlated replies. A publisher timeout remains uncertainty;
it is never converted into a rejection or cancellation.

AddOrder is accepted only after a recomputed canonical authorized intent (with
payload, participant, identifier, policy, and profile fingerprints) binds the
encoded command byte-for-byte and the target-specific execution-safety receipt
has been atomically published. Loopback alone is not treated as evidence of a
fake runtime.
The receipt is derived from the current committed projectors immediately before
the post and contains the target BBO, local monotonic age, exchange evidence,
session/instrument state, exact client limit-row hash, stream readiness,
publisher/reply-listener state, trading capability, passive-price and distance
checks, and quantity-one policy. DelOrder and exact-ext recovery deliberately
skip this entry gate so cleanup remains available after market-data staleness.

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
instrument traffic cannot refresh the target. A missing, one-sided, deleted,
stale, non-tradable, or non-target instrument is not a ready target and must
not authorize a post. Target readiness also requires current-day
`fut_sess_contents` membership, current session and instrument status rows, a
futures min-step, the exact session in state `1` (running; Add + Cancel
allowed), every required private stream and AGGR20 epoch online and
snapshot-complete, exactly one matching seven-symbol client limit row with
`limits_set=true`, and (when requested) exactly one matching client position
row with `xpos=0`.

## Profile and ABI controls

The private TEST profile uses
`${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}` in `env_open_settings` and models
`MOEX_PLAZA2_CGATE_SOFTWARE_KEY` separately from the exchange credential
variable. The private runner rejects the exchange credential token in the
CGate software-key setting. No secret value is stored in Git.

The runtime adapter continues to use the locked MOEX CGate 9.3 ABI:
`CG_MSG_DATA` and `CG_MSG_P2MQ_TIMEOUT` are decoded through the current
`cg_msg_data_t` layout, including `owner_id`; no ABI layout was changed.

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
- two instrument AGGR20 isolation and target-only freshness, one-sided/stale/
  absent targets, scheduled/running/suspended/completed session states,
  missing or non-tradable session, missing or wrong client limit row, non-zero starting
  position, marketable and out-of-distance prices, unavailable
  p2mqreply/publisher, and receipt-persistence refusal;
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

The final offline validation was green:

| Check | Result |
|---|---:|
| Full CTest | 155/155 pass |
| PLAZA label | 45/45 pass |
| TWIME label | 74/74 pass |
| Sanitizer label | 66/66 pass |
| .NET ABI checks | 2/2 pass |
| Changed-target ASan/UBSan | 6/6 pass |
| No-test/no-operator minimal build | 57/57 build steps |
| `git diff --check`, source/repo style, Unicode | pass |
| Native offline plan/privacy check | pass |
| MOEX/broker/network/live order activity | none |

Do not start the live TEST transport. This branch is ready for a draft review
PR only; it does not authorize a live order.
