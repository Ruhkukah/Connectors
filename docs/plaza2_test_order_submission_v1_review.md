# PLAZA II TEST order submission V1

## Scope and authorization

Based on post-PR30 main `49d8a4fea9139fdb30607c81cbc5c84505afda02`.
PR30 was merged normally at reviewed head
`29eb19dfe32d71ccc7ec1ea20f7624917a99d24b`, preserving its evidence history.

This increment prepares one passive CRU6 LIMIT contract on the isolated T1
CGate 9.9 stack. Implementation approval is not order authorization. Every
previous plan is retired. A fresh canonical plan and all 64 SHA-256 characters
must receive explicit human authorization before its intent is installed and
the lifecycle controller is invoked. No actual order has been authorized or
submitted as part of this implementation validation.

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

Live read-side qualification and exact plan authorization are still pending.
This document does not claim a successful real order lifecycle.

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
