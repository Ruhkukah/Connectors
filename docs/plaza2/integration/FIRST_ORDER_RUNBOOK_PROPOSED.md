# Proposed first order runbook — review required

This is a future proposal, not permission to connect or send. No dated harness is created. Review all three
draft PRs and grant separate live authority before preparing any executable live scenario. Use the persistent
ConnectorHost API, not the one-shot lifecycle runner. There is no deliberate router fault in this first-order
scenario.

1. Freeze source, binary, runtime, scheme and protected configuration hashes. Check current MOEX T1 notices,
   exact fresh target/session, account and PART identity. Require complete effective readiness, fresh
   POS-to-TRADE/UOB evidence, zero position, zero active orders and no unresolved epoch. Retain source
   generations and timestamps.
2. Propose one ordinary instrument and one lot, with side explicitly reviewed. Select the price from fresh
   exact exchange bounds and live uncrossed BBO at run time. No price is hard-coded here. Work in integer tick
   units: round the lower bound inward/up and upper inward/down. Intersect that range with prices two to four
   ticks behind the same-side BBO and at least two ticks inside each exchange boundary. Choose the
   farthest-from-market tick within that intersection. For a buy, behind means below bid; for a sell, above
   ask. Reject an empty intersection. The current reviewed distance ceiling is four ticks; do not widen it
   silently. This is the most passive selection under that cap, not a claim of guaranteed non-execution or
   arbitrarily deep placement. Lower-bound-plus-one is not the selection rule.
3. Generate a canonical plan binding target/session, side, exact price, quantity and committed
   terms/provenance/generation. The operator approves its exact hash. Recheck effective gates, freshness and
   unchanged terms immediately before Add. Changed authority requires a new review; do not silently recompute
   or resend.
4. Invoke one Add and preserve allocation/post certainty, correlated exchange reply and exact private Working
   evidence. Then invoke the exact ordinary Cancel, retaining reply and terminal private proof. Verify zero
   active orders and zero position before classifying the scenario PASS. Publisher/reply success alone is
   insufficient. If a fill occurs, stop the scenario with its actual outcome and request separately authorized
   exposure handling; do not flatten automatically.
5. If incidental transport loss occurs, retain the epoch and allow bounded fresh rebootstrap. Polling never
   sends. Reconcile the exact order; if positive terminal proof exists, close without another command. Only
   one exact surviving Working order can produce a new protected operator-cancel artifact. Obtain approval of
   its exact hash before one DelOrder. If that Cancel is uncertain after another loss, reconcile and obtain
   new approval; never resend automatically. A process restart with a nonterminal checkpoint remains outside
   this new in-process cancel surface and requires separate operator handling.
6. Seal the complete result with all hashes, scenario and T1 timestamps. Retain every failure or unresolved
   result. Do not promote live lifecycle or Working-order outage status from offline results or the accepted
   zero-order run.
