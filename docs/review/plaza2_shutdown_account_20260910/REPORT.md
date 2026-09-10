# Qualification shutdown and participant classification

This patch is stacked above PR #49 source `e8f4768ced280f937af0665d25496308ec0fb0fc`.
PR #47 (`8c355979c9c24ebbce558c24a1356587ea8a25bd`), PR #48
(`f966a7348a3252db98c8e9e363e7b6005e009e99`) and PR #49 remain unmerged.
Main is `82707d4e00b1bb6bf703be800ff31988990116df`.
No T1 connection, order, router fault or deployment is authorized in this tranche.

## Historical evidence binding

[Machine-readable binding](historical_binding.json) retains exact source/binary/runtime/scheme identities,
archive SHA and constituent evidence hashes. All 28 archived evidence/journal files were verified read-only.
The original September 10 result remains **FAIL / exit 3**. Nothing in that evidence was rewritten.

At 09:41:54.231610 MSK the operator sent SIGTERM. At 09:41:54.232090 the vendor reported
`poll failed; err 4-'Interrupted system call'`, followed by `COMMON:SYSCALL_FAILED` and connection ERROR.
The host preserved `cg_conn_process: CG_ERR_INTERNAL`, runtime code 131072, operation poll and causal states.
This is an asynchronous signal-delivery defect in the qualification process, not grounds for suppressing
CG_ERR_INTERNAL in production recovery.

Independent observations remain valid: 658.708 stable seconds after full bootstrap; 205,640 callbacks;
15,087 AGGR commits; zero invalid books, callback errors, event loss, orders, positions, publisher posts
and active order epoch. Effective failure masking passed: raw private/AGGR snapshots remained ready,
but effective private/AGGR/publisher/reply and order readiness became false after the failure.
`state_after.json` was recorded before host.stop(), despite its filename.

Price facts are frozen: ALRS-9.26 / ALU6, isin 4519447, session 11703, replID 131, replRev 1146,
LifeNum 101386965. Both limit fields 184; both settlement fields 2077; deposits 367.90/369.50;
BBO around 2042-2045. All 16 generic/independent field conversions agreed. Status remains
**MOEX_CLARIFICATION_REQUIRED**. No settlement-plus/minus formula was introduced.

## Participant identity

Length 4 is brokerage-firm-shaped; length 7 is client-shaped; other lengths are Unknown.
The historical seven-character row 186438 is consequently **CLIENT_SHAPED_NO_EXACT_MATCH** under the
corrected interpretation. The old artifact's Unknown value remains untouched. Structural kind does not
establish the account: exact comparisons use original protocol bytes, with no trimming, case conversion,
normalization or substitution. The observed row still equals neither candidate, so participant_identity_exact
and new_order_allowed remain false. limits_set interpretation/gating is unchanged.

For a future explicitly authorized run, `MOEX_AGGR_PRIVATE_IDENTITY_PATH` enables a one-shot private
artifact after private synchronization. It is created exclusively with O_NOFOLLOW/O_CLOEXEC and mode 0600;
existing files/symlinks are rejected. At most 64 subscribed PART rows and 32 bytes per identity are accepted.
Exact row/broker/full-client bytes are hex encoded to preserve non-ASCII values. Hex is not privacy protection;
file access control is mandatory. Public participant output remains length/kind/equality only for identity.
Default qualification snapshots do not copy the added raw-code field. There is no normal polling logger.
The artifact is empty if synchronization never completes; that is not account evidence.

The currently subscribed user table has generated field descriptors, but this host does not materialize a
current-login user record or expose an independently identified login key for a targeted query. This patch
therefore does not add a generic directory collector. user.client_code supporting context is NOT_COLLECTED;
it must never independently authorize an order. Raw private evidence must not be committed or attached to
public support issues. The private artifact is opt-in and does not bypass the existing date/auth guards.

## Shutdown mechanism and regressions

Before CGate resources or vendor threads start, the single owner blocks SIGINT/SIGTERM with
pthread_sigmask. Vendor threads inherit the mask. Between owner polls, sigpending/sigwait consume pending
stops; the loop exits before another host.poll(). Signals remain blocked throughout same-owner finalization,
host.stop() and exception unwinding. No asynchronous stop handler runs inside the vendor call.
The existing nonzero process timeout, error propagation and recovery algorithm are unchanged.

Subprocess tests send real SIGTERM and SIGINT during an idle blocking poll, heavy callback activity and
immediately after a commit. A real POSIX poll syscall must complete without EINTR. Tests require one owner
poll, one teardown, no next poll/recovery, and successful exit. An independently injected processing failure
remains exit 3. The existing actual ConnectorHost/fake-CGate integration also injects CG_ERR_INTERNAL and
requires exact cause retention, Failed state, no reconnect and zero publisher calls. A structural runner test
proves no order-request ingestion or Add/Cancel API path. These are offline regressions, not native vendor
shutdown qualification. The pending-signal check adds no heap work or busy-poll retry loop.

## Proposed next live scenarios -- not executed

First, after separate authorization, a bounded zero-order/zero-position observation with the exact new
source/binary hashes, fresh committed target session and new evidence/journal. Explicitly opt into the
protected account artifact. Validate graceful SIGTERM shutdown; validate SIGINT in a separate fresh run.
Retain each result under its own identity, with no date-guard bypass or reinterpretation of September 10 FAIL.
If scheduled after September 10, a separately reviewed date-specific harness change is required.

Only after shutdown qualification, separately authorize a zero-order/zero-position router stop/restart
scenario for PR #48's transport recovery: healthy streams -> Recovering -> fresh POS anchor -> TRADE replay
and all streams/AGGR/publisher/reply healthy, no process exit or posts. Account/price business readiness may
remain blocked during this transport-only test. It is not a prerequisite for testing transport recovery.

Live orders remain unauthorized. Preserve `POST_RECOVERY_OPERATOR_CANCEL_NOT_IMPLEMENTED`; no Add with
recovery enabled, no automatic resend/Cancel/flatten. AGGR and private-state performance refactors are not
part of this branch, and no performance measurement/deployment claim is made.
