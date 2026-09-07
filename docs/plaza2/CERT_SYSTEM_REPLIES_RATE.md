# Offline certification fixes: system replies and publisher pacing

The existing reply bridge now accepts 99/100 for each armed command/user_id, while retaining its unknown-correlation,
wrong-family, malformed and contradictory-duplicate checks. The transport preserves message ID, code, text, raw bytes and
flood penalty. Native lifecycle observations have an explicit `ambiguous` bit; no C ABI layout changes are involved.
The existing journal records message ID, ambiguity and raw hexadecimal payload for each operation's reply.

Official 9.9 sources are the CGate/SPECTRA manual and `forts_messages.ini` recorded in the public wire evidence lock:
FORTS_MSG99 means flood denial before delivery to the trading core. It is a rejection, not acceptance despite its absent code.
Its `penalty_remain` is milliseconds and delays further locally admitted messages. FORTS_MSG100 is a system delivery/handling
error and remains ambiguous, even for a malformed-semantic zero code. It cannot imply that an order was rejected or cancelled.
The existing bounded lifecycle deadlines and factual reconciliation remain in effect; at most the already authorized exact-ext
cleanup is available. System responses never trigger another AddOrder. Actual timeout telemetry remains separate from ambiguity.

`Plaza2TestSessionHostConfig::publisher_messages_per_second` configures the local publisher gate, also exposed by
`Plaza2HostConfigInputs`. Default 30 is a conservative local setting, not a claim about the exchange-provisioned login limit.
The native setting accepts 1..3000 to bound storage. Configure it from the actual provisioned cap, accounting for other processes
sharing the login. C ABI layouts and command-line configuration are unchanged; this is configured through native host inputs.

The single-owner gate retains a bounded ring of admission timestamps for the rolling preceding second. At exactly 1000 ms a
prior attempt expires. It counts admitted attempts conservatively, including allocation/post failures, with no refunds, sleeping,
automatic retry or resend. A throttled command remains DefinitelyNotSent and does not call the publisher. Clock regressions deny
admission. Rate and flood-penalty state survive host stop/start and publisher/listener/environment reopen within the same host.
It does not persist across process destruction; independent processes require operational coordination of their provisioned cap.
Metrics expose cumulative admitted/throttled counts, clock regressions, penalty deadline and occupancy as of the last decision.

Deterministic tests cover rolling-window boundaries, maximum/invalid configuration, penalty expiry, backwards clocks,
full host reopen without resetting the budget, raw 99/100 callback handling, code-zero system ambiguity, and bounded lifecycle
resolution without automatic AddOrder resend. All tests use fake/local publishers. No exchange orders or T1 access occurred.
