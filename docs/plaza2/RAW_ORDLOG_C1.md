# Raw anonymous ORDLOG, C1

`Plaza2Ordlog` is a native, single-owner public receiver on the existing `Plaza2Connection`/`Plaza2Listener` runtime.
Create it with the official `ordLog_trades.ini` path; it opens `p2repl://FORTS_ORDLOG_REPL` with the complete `CustReplScheme`.
It is separate from AGGR20 and never creates a publisher. All four tables are delivered: `orders_log`,
`multileg_orders_log`, `heartbeat`, `sys_events`. ORDBOOK's six tables are wire-qualified but have no C1 projector.

## Wire and dispatch

The raw opt-in validates all table names, order, sizes and every field name/type/offset/size against the Phase B lock on every OPEN.
The existing callback bridge then forwards borrowed wire/null spans without generic string/decimal/timestamp conversions.
The receiver validates size, index, null map and exact BCD format before copying into preallocated record slots.
`OrdlogRecord` retains the complete wire bytes, callback null map, table identity, service fields, life, generation, receive
sequence and monotonic poll timestamp. Its stream is always ORDLOG. Prices are BCD, timestamps retain milliseconds and
`moment_ns` remains unsigned 64-bit. The generated structs expose the exact current public names; fields absent from the public
scheme (such as `ext_id`) are not invented. Unknown payload flags/actions are not rejected.

`public_wire::load<T>` requires a previously validated span and a qualified field offset. It uses memcpy to avoid unaligned
loads and does not synthesize missing values. No managed callback, per-record allocation, log formatting or clock syscall occurs.
Call `poll_time()` once before the owner's connection poll. All APIs, including acknowledgements, run on that same owner thread.

## Visibility, backpressure and recovery

A fixed-capacity ring holds both committed output and the current CGate transaction. Output becomes visible only at TN_COMMIT.
A transaction exceeding available space fails explicitly; mandatory overflow makes health Failed and clears checkpoint eligibility.
Partial transaction rollback is counted separately because CGate must retransmit it. Committed pending output is never discarded
by reopen: the owner must finish processing and acknowledge it first.

An optional second fixed-capacity queue receives the same committed records. On overflow it permanently marks itself overflowed,
records the first lost sequence and counts subsequent omissions. It continues exposing its retained prefix. This is an incomplete
subscription, not a healthy feed; construct a new receiver for a new optional subscription. Mandatory processing is unaffected.
There is no generic subscriber manager, disk journal, unbounded backlog or second connection thread.

The checkpoint string is an opaque CGate token, not an invented revision cursor. REPLSTATE is eligible only after TN_COMMIT,
with no incomplete transaction, no failure, and all mandatory output acknowledged. `acknowledge()` means the owner has completed
its mandatory processing. A consumer adding persistence must atomically persist its own processed state with that eligible token;
acknowledging a merely enqueued asynchronous write is invalid. C1 itself writes no durable checkpoint. A new process therefore
starts from fresh history. The tests prove that a marker is unavailable before acknowledgement and that the exact eligible marker
is passed to CGate on a drained same-process reopen, with replay retained. A later marker cannot erase a callback/overflow failure.

CLOSE immediately makes health Stale and rolls back uncommitted output. `supervise()` notices CLOSED/ERROR and retries after
one second, only when the shared connection is ACTIVE and mandatory committed output is drained. Connection reconnection remains
owned by the existing connection owner. Scheme/decode/revision/mandatory overflow failure requires explicit `open()` recovery,
which forces fresh history; it is not an endless automatic retry loop over corrupt data. No gap is silently covered by a marker.

LifeNum changes invalidate the stream generation and online readiness. The transition is itself ordered output, carrying life and
generation. ClearDeleted retains the affected table, boundary and flags; only CG_MAX_REVISON resets its revision tracker.
C1 performs no book-row deletion. ONLINE plus mandatory queue drain establishes raw Online; it says nothing about current L3.

## Revisions

Revisions are tracked separately for all four tables. Increasing, observed numeric discontinuity, legal history replay,
consecutive identical duplicate and generation change are distinct. Every accepted record, including duplicates and multileg,
is retained. Equality compares protocol fields/nulls, excluding padding. History may be nonmonotonic; a numeric jump is observable
but does not prove transport loss. Online conflicting/backward revisions fail closed. Only the preceding row per table is retained
for duplicate classification; this is deliberately not an unbounded historical deduplication index. Older history records remain
replay, never silently removed. Mandatory loss, malformed data and revision corruption have separate counters and unhealthy state.

## Qualification boundary

The deterministic test drives both direct recovery scenarios and actual dynamically loaded CGate callback dispatch through the
fake library. It covers all four layouts, malformed indices/types/BCD/nulls, duplicate/replay/discontinuity, LifeNum, maximum
ClearDeleted, transaction rollback, mandatory/optional overflow, eligibility before/after acknowledgement, listener loss,
bounded reopen, exact continuation token and scheme mismatch. No T1 connection or exchange order is involved.

This is raw C1 only. Composite p2ordbook fixtures, full identity/mutation invariants, multileg synchronization and the complete
L3 performance path remain C2 gates in `P2ORDBOOK_ORDLOG_RECOVERY_9_9.md`.

Local Release validation: 169 registered tests. All PLAZA tests and preflight checks passed. An unchanged TWIME establish-deadline
fixture intermittently failed (`establish timeout must fault rather than silently hang`), then passed on an isolated rerun.
That timing-dependent fixture is not treated as a clean full-suite pass; no TWIME production behavior was changed for this tranche.
