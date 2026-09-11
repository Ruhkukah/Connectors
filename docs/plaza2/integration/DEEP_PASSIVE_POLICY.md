# First-order deep-passive policy (offline)

FIRST_ORDER_DEEP_PASSIVE_V1 is opt-in through `first_order_deep_passive_policy()`. Legacy/test policies and their
max-distance fields retain their existing behavior. V1 has a fixed definition hashed into every authority:
quantity=1, inset=2 ticks, minimum same-side gap=20 ticks, maximum BBO age=1000 ms. Existing max_distance_ticks
remains four in configuration; V1 uses its own minimum-gap validation, not a widened legacy maximum.

The native proposal returns both candidates, exact scale-100000 prices, integer gaps and a preferred side.
It does not authorize or post. Buy is ceil(lower/tick)*tick + 2*tick; sell is floor(upper/tick)*tick - 2*tick.
Invalid/missing bounds, nonpositive or non-tick BBO, locked/crossed/stale quotes, overflow or insufficient gaps
produce no eligible side. Greater valid gap is preferred; ties prefer buy only as a proposal.

The canonical Add plan binds V1 version/SHA and reviewed BBO values, AGGR stream/revision, monotonic commit time
and exchange timestamps when supplied. Existing exact session-term binding still binds target/session,
LifeNum/revision and transport generation. Deep-policy authority validates its reviewed candidate; immediately
before publisher allocation/post, current terms must still match exactly and the current fresh BBO must retain
at least 20 ticks of cushion. A safe BBO move may retain authority; a lost cushion invalidates it without moving
the order. AGGR's internal scale differs from session decimal units: this boundary parses the exact textual
values into scale 100000 and never mixes the two representations or uses double.

A fill observed under this policy latches FIRST_ORDER_FILLED and disables subsequent Add authority for the
owner even after epoch finish. Cancellation of an exact remainder remains explicit; no flatten or automatic
new order is introduced. The proposed runbook defines separate human handling after evidence sealing. No deep
price can guarantee non-execution.

Offline tests cover both historical ranges, inward rounding, inclusive 20-tick and 1000-ms limits, stale/future
and crossed/locked BBO, invalid/missing/overflow terms, insufficient cushion and rejection of substitute prices.
Native fake-runtime tests prove unchanged legacy behavior, both proposals, canonical binding, successful exact
approval, BBO movement during preflight with zero allocation/post, and the fill latch without a cleanup order.
All prior restart/recovery/send-gate suites remain in the final stack. No new live claim is made.
