# September 11 PR55 zero-order recovery harness

Functional base: exact reviewed PR #55, e004251ae932a2d15736ecf3bc07d03fcf63d160.
Historical PR #54 source and evidence remain unchanged. This new executable is
plaza2_aggr_qualification_20260911_pr55, with a fixed September 11 authorization
window 06:58-16:10 MSK and token 20260911_PR55_AGGREGATED_OBSERVATION.
Run/profile suffix is 20260911-pr55; ext/user IDs start at 2026091150.
Any order authorization environment variable is rejected, including empty values.
No runtime date override or order-enabled entry point is introduced.

Only date, run identity and packaging differ from the reviewed observation harness.
The PR55 recovery implementation and all account/price/private/AGGR/shutdown
semantics remain byte-identical to the reviewed functional base.

Build native x86-64 Release on Ubuntu 22.04 from the exact PR head, not the
synthetic merge SHA. Record binary/runtime/scheme/config hashes before transfer
and verify them remotely in a new immutable source-SHA directory. Recheck MOEX
T1 availability before deployment and launch. Use fresh evidence/journal roots.
Rediscover the committed current ALRS-9.26/ALU6 identity; do not assume session 11704.

Require fresh PART exact match 1, limits_set 1, POS to TRADE flatness, zero own
orders, zero active epoch/posts, and a dedicated router before a stable 180-second
baseline. Stop that router once, retain process INTERNAL and at least one bounded
ConnectionOpen INTERNAL/INVALIDARGUMENT failure while down, then restore the exact
router unchanged. Prefer two failed opens if comfortable within the deadline.
Do not start the fault after 15:30 MSK. Never restart the connector to claim PASS.

Require same PID, fresh bootstrap provenance and restored effective health,
then 180 seconds stable observation and one graceful SIGTERM, exit 0.
On any failure restore the router, seal evidence and stop without patch/repeat.
No exchange orders, merges or heartbeat. The identity-match boolean for a
recovery-time INVALIDARGUMENT is an inference from the exact reviewed policy's
successful exception branch; the internal secret-derived digest is never exported.
If direct native session terms are not already exposed, retain raw field evidence
and label any derived calculation separately. No new source instrumentation.
