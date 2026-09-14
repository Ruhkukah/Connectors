# Proposed first order runbook — review required

Future proposal only: no authority to connect or send and no dated harness. Review and merge the durable
#57→#58→#59→#60→#61 stack first, then separately authorize harness preparation and one live test. Use persistent
ConnectorHost, never the one-shot lifecycle runner. No deliberate router fault and no artificial resting delay.

## Preconditions and deep-passive proposal

Freeze exact source, binary, runtime, scheme and protected configuration hashes. Check current T1 notices and
fresh target/session/account/PART identity. Require effective readiness, fresh POS→TRADE/UOB evidence, zero
position, zero active orders and no unresolved epoch. The only planned quantity is one ordinary contract.

Select the narrowly named `FIRST_ORDER_DEEP_PASSIVE_V1` policy using `first_order_deep_passive_policy()`; configure
the transport freshness ceiling to 1000 ms too. Existing legacy smoke policy and its four-tick ceiling stay
unchanged. V1 binds boundary inset=2 ticks, minimum passive gap=20 ticks, BBO maximum age=1000 ms and quantity=1
in its exact definition/SHA. Any parameter change requires a new policy version and review.

From current committed authoritative session terms, use exact scaled integers:

- `tick_lower = first tick >= lower`; `candidate_buy = tick_lower + 2 * min_step`.
- `tick_upper = last tick <= upper`; `candidate_sell = tick_upper - 2 * min_step`.
- `buy_gap_ticks = (bid - candidate_buy) / min_step`.
- `sell_gap_ticks = (candidate_sell - ask) / min_step`.

Require fresh positive uncrossed tick-aligned BBO and at least 20 ticks on the selected side. Show both candidates
and their valid gaps. Prefer the greater valid gap (buy on an exact tie), but do not automatically choose and
send. If neither qualifies: `DO_NOT_RUN`. Missing/overflow/invalid/narrow bounds fail closed.
For the historical 2045/180/180 regression with min_step=1, the candidates would be 1867 and 2223. These are
regression numbers, never a live hard-coded price. Deep placement reduces fill probability; it cannot eliminate
execution during an extreme market move.

## Exact operator authority and sequence

The operator reviews side, exact price, quantity=1 and canonical plan hash. The plan binds policy version/SHA,
minimum gap, maximum BBO age, reviewed bid/ask, source revision and local commit/exchange timestamps, alongside
exact session terms/provenance/generation. Immediately before Add, require unchanged exact terms, effective
health and current BBO <=1000 ms old, with the selected price still at least 20 ticks behind the same-side BBO.
A lost cushion invalidates authority. Never move the price, refresh terms or resend under an old approval.

One Add → correlated reply → exact private Working → operator immediately invokes exact Cancel → reply → fresh
private Cancelled proof → zero Working orders → zero position → graceful shutdown. Do not insert a resting
period. Preserve msgnew/post certainty, replies, raw/derived terms and private evidence. A reply alone is not
terminal proof and an absent row alone is not terminal proof.

## Accidental-fill state machine and independent operator procedure

Before Add, name the human responsible for any one-contract T1 position and confirm access to an independent
MOEX/broker-approved T1 terminal or operator channel for this exact account. If unavailable, `DO_NOT_RUN`.
The person must understand the contract, account and session and be able to obtain current authoritative
position/order evidence independently of this connector. This procedure grants no automatic flatten authority.

- Partial fill with an exact remaining Working order: the operator invokes the normal explicit exact Cancel
  for that remainder as soon as possible. Confirm its outcome, freeze further Add authority, record actual
  nonzero position, and stop the qualification scenario. For an indivisible one-contract order, a fractional
  partial fill is not expected; unexpected quantity evidence is an incident, never permission for another Add.
- Fully filled: no Cancel for a terminal order and no automatic subsequent order. Freeze new-order authority,
  preserve exact position/deal evidence and classify `FIRST_ORDER_FILLED`. The one-lot Add bounds possible
  unplanned exposure to one contract; no second Add is permitted in this scenario.
- Seal evidence first. The named human then verifies the account, remaining orders and net position in the
  independent T1 terminal/channel, obtains separate explicit authority to handle that observed position, and
  performs the reviewed manual operation there. They verify final exchange position/order state and append a
  separately attributed result. If that route is unavailable or ambiguous, keep the connector no-Add and
  escalate to the designated broker/MOEX operator. Do not send an unreviewed market flatten from this harness.

## Process and transport contingencies

If the owner stays alive but transport fails, use qualified bounded recovery and PR59 fresh exact reconciliation
plus new explicit recovered-Cancel approval. If the process dies, use PR60 v2 restart recovery-only mode: fresh
full bootstrap, exact historical session/account identity, fresh proof or new approval. Never reuse a prior
artifact, reply ID or Add authority for a resend. Another uncertain Cancel requires another fresh reconciliation
and new explicit approval. Legacy nonterminal v1 or incompatible session/account remains blocked.

Seal every outcome with source/binary/runtime/scheme hashes, scenario and T1 timestamps. Retain all historical
FAILs and exact source identities. No live lifecycle or Working-order outage PASS follows from offline tests.
