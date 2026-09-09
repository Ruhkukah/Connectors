# Bounded TEST transport recovery

This change is stacked on PR #47 (live truth and diagnostics), whose base is
`82707d4e00b1bb6bf703be800ff31988990116df`. Historical T1 executable evidence remains
identified by `96199b6ca5b1141c01670830be067bfd69c5ed71`. The old router-loss FAIL is
unchanged. All recovery results here use the deterministic fake runtime, not T1.

## Ownership and transitions

`Plaza2TestSessionHost` owns one operational lifetime, separately from exchange
session status: Starting, Running, Recovering, Failed, Stopped. Recovery reuses
its existing resource startup/teardown. It does not restart the process or create
a second owner thread. The default retry interval is one second and the deadline
is sixty seconds. Both are native configuration fields; intervals below one
second and deadlines no greater than the interval are rejected. Tests inject a
monotonic clock. A reconnect attempt includes the complete fresh bootstrap, so a
connection which opens but never synchronizes cannot extend the deadline.

A CLOSED/ERROR connection or a previously synchronized private listener triggers
rebootstrap. After full readiness, AGGR, publisher and reply loss do so too.
Only transport-state errors with defined semantics are retried. Runtime internal,
unsupported/unknown, schema, decoder and callback errors remain fatal. The runtime
listener now exposes its retained callback error, including AGGR decoder failures;
a callback-induced listener ERROR cannot be misclassified as router loss.

On loss, effective health and sending become false immediately. Resources are
invalidated, the prior cause and object-state census retained, and the next attempt
waits for its monotonic deadline. Fresh private replication selects a new immutable
POS anchor and opens TRADE replay from it. Independent reference/status/PART/UOB
and AGGR subscriptions bootstrap afresh. Publisher and reply reopen only after
fresh replication is complete. Readiness requires all handles ACTIVE and current
snapshot/ONLINE validity. A changed POS revision/LifeNum is tested explicitly.

Qualification logs emit structured state on recovery transitions and attempts,
including generation, attempt count, original error, runtime code, failure time
and transport census. They do not log every unchanged poll. These records describe
new runs only; no historical evidence is rewritten.

## Interrupted orders

Transport resource replacement does not reset the persistent controller, epoch,
Add/Cancel attempt latches, or publisher invocation counters. No recovery path
calls Add, Cancel, cleanup or flatten. Incomplete replacement snapshots produce
no order observations. Missing rows are not interpreted as terminal orders.

An epoch with a possibly sent Add is bound to its transport generation. After
loss it can rebuild observation and reconcile, but cannot issue another command
from that interrupted epoch. This deliberately also blocks automatic lifecycle
cleanup after reconnection. A reviewed safe terminal result is required before
normal epoch reset can make a new order eligible. The qualification application
blocks its automatic cancel/order sequence after a transport incident.

Tests inject loss after Add `post_invoked` before its reply, while Working, and
after Cancel `post_invoked`. All retain the active epoch and prove unchanged
independent publisher counts through rebootstrap and repeated polling/submission
attempts. No live order is part of this change.

## Validation and remaining live gates

The ConnectorHost fake integration test covers initial-router loss, ready-router
loss, connection CLOSED/ERROR, one private listener ERROR, AGGR ERROR,
publisher/reply invalidation, two failed reconnects followed by success, deadline
exhaustion, fresh LifeNum/POS-to-TRADE anchor, three interrupted-order stages, and
fatal private callback/AGGR decode corruption. Existing non-recovering tests retain
the old failure behavior with recovery disabled for that explicit reproduction.

Release correctness and sanitizer results must be reported separately from any
performance evidence. There are no performance claims or large state/AGGR
refactors in this PR.

Before deployment, review this PR and PR #47. Actual T1 account identity and raw
179/179 field interpretation remain unproven. The first separately approved live
run must be observation-only; the second is zero-order/zero-exposure router
stop/restart. Live order testing with recovery enabled is not authorized under this
implementation, even after those observation scenarios pass.

## Runtime result classification regression

The added fake case makes `cg_conn_process` return `CG_ERR_INTERNAL` (131072 /
0x20000) while the connection changes from ACTIVE to ERROR. The wrapper reports
`RuntimeCallFailed` with the exact message `cg_conn_process: CG_ERR_INTERNAL`.
The host enters Failed, keeps all effective gates false, retains the original
runtime cause and ERROR census, and performs zero retries or publisher calls.

This remains fatal deliberately. MOEX's [CGate manual, section 2.5.5](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/Game/docs/cgate_en.pdf)
describes this result as an internal error which may reflect configuration or
runtime-environment faults and calls for library-log diagnosis. An ERROR handle
alone does not prove that rebootstrap is safe. This test does not claim to model
all vendor router-disconnect results. The existing real `CG_ERR_INCORRECTSTATE`
(131077 / 0x20005) with CLOSED/ERROR remains recoverable; tests now assert that its
exact `cg_conn_process` result is preserved before any synthetic AdapterState check.
Unknown, unsupported, schema, decoder and callback categories remain fatal.

## Explicit future order gate

`POST_RECOVERY_OPERATOR_CANCEL_NOT_IMPLEMENTED`

`LIVE ORDER TEST WITH RECOVERY ENABLED = NOT AUTHORIZED`

A Working order crossing a transport generation cannot currently be cancelled by
the recovered epoch. A separately reviewed, explicit operator-authorized
post-recovery Cancel mechanism is required before live order tests with recovery
enabled. No automatic Add, Cancel, cleanup or flatten policy changes are included.
This gate does not block observation-only testing or a controlled router test with
zero orders, zero positions and no active order epoch.
