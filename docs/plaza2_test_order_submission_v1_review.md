# PLAZA II TEST order submission V1

## Scope and authorization

Based on post-PR30 main `49d8a4fea9139fdb30607c81cbc5c84505afda02`.
PR30 was merged normally at reviewed head
`29eb19dfe32d71ccc7ec1ea20f7624917a99d24b`, preserving its evidence history.

This increment prepares one passive CRU6 LIMIT contract on the isolated T1
CGate 9.9 stack. Implementation approval alone is not order authorization.
The initial work order required separate exact-SHA approval; the user later
superseded that handoff by explicitly delegating fresh plan selection and this
one TEST exercise, as recorded below. Prior retired plans remain retired.
No exchange order was submitted; the attempted lifecycle stopped before post.

## Bounded send boundary

- `LiveTestPreSend` retains its physical `DefinitelyNotSent /
  SEND_DISABLED_PRE_SEND_PHASE / post_invoked=false` return before allocation.
- New `LiveTestAuthorizedSend` requires TEST, the explicit local T1 endpoint
  `127.0.0.1:4101`, POS-anchored TRADE replay, all existing TEST arms, and the
  separate `test_order_send_armed` flag. Unknown modes fail closed.
- The public session-host post API refuses this mode. Only the validated trade
  transport can reach its private publisher handoff.
- Existing exact intent, canonical plan binding, payload, quantity-one,
  provenance, status, fresh uncrossed BBO, limits, position and UOB gates remain.
  This mode additionally requires zero position, at most four ticks and at most
  5000 ms quote age; these cannot be relaxed by its plan.
- Before allocation, the existing atomic local receipt writer persists v4,
  which adds send mode and arm evidence. Other modes retain v3 JSON. No
  power-loss durability is claimed.
- One Add attempt maximum per transport, including a definitely-not-sent
  failure. Cleanup requires an Add that may have existed. One DelOrder and one
  existing exact-ext recovery attempt maximum; no new retry machinery.
- DelOrder must encode the authorized account/instrument and the unambiguous
  correlated Add reply order ID. The existing lifecycle decides whether to
  cancel, recover, or stop; no separate smoke publisher is introduced.
- Late intent installation supplies lifecycle ext/client identity. TRADE is
  lifecycle evidence; TEST USERORDERBOOK remains only the independent current
  own-order census. Their disagreement does not manufacture an incident.
- No production, TWIME, C ABI submission, or compensating order support added.

## Offline evidence

The existing transport scenario test now instruments actual fake function
entries independently of the application's publisher counters. A valid armed
Add reaches `cg_pub_msgnew=1`, `cg_pub_post=1`; configuration, authorization,
market/account and receipt-write refusals reach `0/0`. The test also checks
public-host bypass refusal, second-Add refusal, allocation failure without
retry, and late-intent controller paths for fill, working/cancel and exact-ext
recovery after an Add reply timeout. Ordinary identity consistency and journal
lock handling remain the existing lifecycle implementation.

Offline validation (2026-09-06):

| Check | Result |
| --- | --- |
| Full CTest | 158/158 passed |
| PLAZA label | 48/48 passed |
| Sanitizer label, regular build | 66/66 passed |
| ASan/UBSan build, union of PLAZA and sanitizer labels | 109/109 passed |
| ABI, no-send, source/style, Unicode | Passed in full CTest |
| Changed C++ formatting and `git diff --check` | Passed |

ASan/UBSan ran with `detect_leaks=0` because macOS does not support this build's
leak detector; undefined-behavior errors halt the run. The sanitizer build's
Python tooling was pointed at the same existing PyYAML-capable interpreter as
the regular build. Initial unsupported-leak/Python-dependency invocations were
environment setup failures, not test passes. No source fix was needed.

## Live delegated exercise: publisher preparation blocked

The user subsequently explicitly delegated fresh price/plan selection without
another authorization round. The temporary operator driver applied that
delegation to a newly generated qty-one passive CRU6 plan on the same pumped
host; repository runtime code and every execution gate remained unchanged.
The earlier manual-authorization candidates were retired without allocation.

Implementation: `39b1a39314be5a7166de0004c43d72255b455f49`; both GitHub jobs passed.
Session 11700, SELL 1 at 12.91500, generation BBO 12.91000 / 12.91400.
Plan: `81e40b34db8d8bdba34377cda06012f7c4d5fc97dc0eaeefe0a097dfa376ad8d`.
Receipt: `1a88c710a9bf426d7b9d261baacdb76cbcb0d374c1f1f09f766dde4609f58182`.

All readiness gates passed; v4 persisted before allocation. Actual application
counts: msgnew=1, post=0. Terminal classification: DefinitelyNotSent,
market_safe_terminal=true, evidence_consistent=true, journal_ok=true.
No Add reply, cancel, recovery, or exchange order. Router PID 3031619 unchanged.
No second Add attempt was made. The controller's ok=true describes the safe
terminal outcome, **not successful order submission**.

Read-only investigation then found a real wire-layout defect. Running the
installed official CGate 9.9 `schemetool makesrc -t AddOrder,DelOrder,DelUserOrders
<official forts_messages.ini> message` produces pack(4) structures sized
128 / 28 / 60 bytes. The connector encodes 112 / 20 / 49 bytes. Its fixed strings
omit the extra terminating byte and integer fields omit structure alignment.
Existing fake fixtures model those same incorrect compact sizes. This is a
confirmed codec/runtime layout mismatch, not a market-readiness failure.

The temporary result summary did not retain allocation_error or
runtime_payload_size, so the exact runtime return and precise immediate refusal
branch cannot be reconstructed from this receipt. Do not claim the allocation
function itself failed: successful allocation followed by the existing
payload-size check is also possible. The confirmed layout defect must be fixed
and independently tested before another submission attempt.

Evidence: `docs/evidence/plaza2_test_order_v1_20260906/`.
PR remains draft; **actual TEST order lifecycle qualification has not passed**.

## First live exercise procedure

Use one continuously pumped host with current authoritative session ID and
fresh market/account/position evidence. Generate a fresh SELL quantity-one plan
one tick above the ask. Persist the canonical plan and show full plan/payload
hashes, IDs, price, BBO and age. Stop for exact SHA authorization; do not create
the authorization file from general permission.

Temporary stale, one-sided or crossed quotes make execution unready while the
same host continues pumping. A fresh book making the static price marketable
or more than four ticks away retires the plan, never reprices it. After exact
authorization, recheck all gates, install the intent and invoke the existing
`OrderLifecycleController`. Filled means stop without an opposite order;
working means one cancel; uncertain means existing exact recovery only.
Unresolved incidents retain identifier locks. Stop after that single lifecycle.

Application `cg_pub_msgnew` and `cg_pub_post` counts are incremented immediately
before actual runtime function calls, not inferred from control flow. They do
not count CGate/router internal traffic. The later live packet must include
these counters, certainty, replies, TRADE observations, independent UOB census,
receipt/journal hashes and router PID before/after.
