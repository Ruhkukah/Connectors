# Native session-price authorization

The host obtains the exact committed FutureSessionTerms for its target instrument/session and current LifeNum.
It exposes five separate price diagnostics. Missing proposed price does not invalidate observation readiness.
OrderTest entry requires valid current terms, exact inclusive bounds and tick alignment.

Canonical plans now bind raw settlement/offset fields and separate derived bounds, min_step, source
stream/table/replID/replRev/LifeNum and transport generation. The Add transport compares the full binding with
authoritative committed state after preflight polling and immediately before publisher allocation/post.
Changed terms require another operator-reviewed plan; no plausibility formula, BBO reference, absolute value
or floating-point conversion is used. Legacy offline plan-only callers can omit a binding, but the concrete
Add transport cannot post without it.

A definitely-not-sent refusal can close its persistent epoch normally. Reauthorization creates a new reviewed
epoch. It does not silently refresh an existing authority or resend an Add. Recovery to another transport
generation invalidates prior price authority even if prices are unchanged.

Validation: full local Release 178/178 and ASan/UBSan 125/125. The fake-runtime test changes replRev during
preflight after authorization, observes zero publisher allocations/posts, closes the refused epoch, rejects
old authority and accepts a new exact authorization without posting. Shared send-validator tests cover both
historical ranges, inclusive endpoints, outside prices, tick alignment, missing values, wrong
identity/session/LifeNum, unhealthy transport, invalid interval, overflow and changed provenance/generation.
Existing projector regressions prove transactional visibility, deletion and ClearDeleted/LifeNum invalidation
feeding this lookup. Linux Release and ASan/UBSan/LSan run on the draft PR.

Live evidence remains attributable to PR56 b0f80c4. This price send gate has offline evidence only.
POST_RECOVERY_OPERATOR_CANCEL_NOT_IMPLEMENTED remains the status at this PR. No live order lifecycle or
working-order outage has been tested. No VPS mutation, T1 connection, order, merge or performance
optimization.
