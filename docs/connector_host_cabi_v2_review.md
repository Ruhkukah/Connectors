# ConnectorHost-backed C ABI V2 review

## Scope

This increment adds an additive C ABI V2 over the merged
`moex::connector_host::ConnectorHost`. It is TEST-only, single-threaded, and
does not connect to the real T1 router or submit a live order. The existing V1
ABI remains unchanged: `moex_phase0_abi_version()` is still `1`, the legacy
replay/private-state surface remains in place, and the placeholder order
symbols still return `MOEX_RESULT_NOT_SUPPORTED`.

## Exported surface

V2 reports version `2` and exports an opaque handle whose native owner contains
`std::unique_ptr<ConnectorHost>`. Create uses a typed fixed request and copies
all strings; credential, software-key, broker, and client fields are
environment-variable names. The only operations are create/destroy,
start/poll/stop, value snapshot, plan info and exact canonical-byte copying,
exact full-SHA authorization, `run_order_test`, and read-only reconciliation.
No raw CGate, publisher, projector, generic AddOrder, cancel, replace, mass
cancel, background thread, or production path is exposed.

All top-level caller-supplied/output V2 structures have `struct_size` and
`abi_version`, explicit reserved space, fixed-width scalar fields,
fixed-capacity text, and exported native `sizeof`/`alignof` functions.
Embedded fixed-layout value records are versioned transitively through their
containing structure. Provenance records retain stream, table,
revision, LifeNum, presence, and typed target row key. Secret values are not
returned in snapshots, plans, errors, or test diagnostics.

## Semantics

Snapshots map the ConnectorHost readiness, market, provenance, POS/TRADE
anchor, independent USERORDERBOOK periodic, position, lifecycle, and publisher
call-count values. The canonical plan is copied byte-for-byte from
`ConnectorHost::plan()`; V2 never parses, reformats, or reconstructs it.
Authorization passes the supplied byte range and full 64-character SHA directly
to `ConnectorHost::authorize()`.

`run_order_test` names the existing bounded one-shot lifecycle; it is not a
general order-submission API. Its fixed result reports lifecycle state,
terminal/evidence/journal flags, submission certainty and `post_invoked`,
correlated replies, identifiers, journal path, and message. A second run is
refused. Reconciliation is read-only and reports the existing restart result;
it never submits a command. `ORDER_TEST` creation requires the caller to
provide the side, price, base contract, positive ext/user IDs, run and
journal/receipt paths, and profile identity; only `QUALIFY` receives inert
builder defaults for those order-specific fields.

## Managed boundary

The .NET adapter contains a low-level V2 library/handle and fixed-layout
interop structs. It validates every native size/alignment export and uses
`byte[]` for exact canonical plans. The managed smoke calls create/start/poll,
snapshot, exact plan copy, authorization, fake OrderTest, reconciliation, and
stop/destroy, including the unload-while-live regression, without wiring V2
into AlorEngine trading logic. The library refuses unload while any V2 host
handle is alive; handles check the library lifetime before invoking a delegate.

## Verification

The native fake-CGate test covers V1 version preservation, every V2 layout
export, null and header validation, copied inputs, qualification with zero
publisher calls, exact-plan authorization (wrong SHA/modified/pretty bytes
rejected), crossed/missing/non-zero-position, active-order, wrong-target and
wrong-session refusals, explicit ORDER_TEST field validation, receipt-write
failure, the fake
Add→Working→Cancel→Cancelled lifecycle, uncertain Add with one recovery and no
Add retry, no second run, and reconciliation. The managed smoke independently
exercises the same ABI and layout contract. Full repository CTest (163/163),
the PLAZA label (52/52), native/.NET ABI smoke tests, and the sanitizer label
(69/69 with Apple leak detection disabled because that runtime does not support
`detect_leaks=1`) pass locally; source style, Unicode, and `git diff --check`
also pass. No live run was performed, and this document makes no
production-readiness claim.
