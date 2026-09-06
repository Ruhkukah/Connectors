# ConnectorHost persistent order C ABI V3 review

## Scope and boundary

This increment adds an additive C ABI V3 and a low-level .NET wrapper over the
merged persistent `ConnectorHost` API. It is TEST-only and fake-CGate-only in
this PR. The V3 handle owns exactly one `ConnectorHost`; it owns no CGate
objects, publisher state, background thread, AlorEngine integration, or raw
message operation.

V1 remains the synthetic/replay compatibility ABI and V2 remains the bounded
plan/authorize/one-shot/reconcile surface. No V1/V2 symbol or structure was
changed.

## V3 contract

Creation describes the runtime, target session, account environment variables,
static safety profile, base run/journal/receipt paths, and host-managed base
identifiers. It intentionally does not accept side, price, comment, ext IDs,
or per-epoch user IDs. The internal config builder receives private inert
construction values only; every executable epoch supplies a copied
`MoexPersistentOrderRequestV3` containing side, price, base contract, comment,
and quantity. Quantity is fixed at one and the existing TEST/LIMIT,
four-tick/five-second, zero-starting-position policy cannot be loosened by the
ABI.

The operations map one-for-one to `ConnectorHost`: start/poll/stop, snapshot,
plan/copy canonical bytes, begin with request plus exact canonical bytes and
full SHA, submit/poll/cancel, explicit finish, and restart reconciliation.
There is no raw submit, arbitrary cancel, replace, or mass-cancel operation.
The handle caches the request and `PreSendPlan`; polling does not rewrite the
cached bytes, while successful begin/finish/stop and reconciliation resolution
invalidate the cache. Native `ConnectorHost` remains authoritative and
recomputes readiness during begin.

Snapshots preserve host/observation state, target and session status, BBO and
age, row-level REFDATA provenance, POS/TRADE anchor and position evidence,
independent USERORDERBOOK periodic state, active-order census, epoch flags,
new-order permission, lifecycle/reply data, quantities, market/evidence flags,
publisher counters, and the last error. Domain results preserve
`ok=false` for expected nonterminal states such as Posted, Working, and
CancelPending; this is not converted into a C ABI transport error. Finish is
never implicit when a terminal result is observed.

## ABI and managed boundary

Every V3 marshalled structure has fixed-width fields, a size/version header,
reserved space, and fixed-capacity returned text. Native `sizeof` and `alignof`
exports cover the create/request, embedded value records, snapshot, plan,
order-result, and reconciliation structures. The .NET library validates all
ten exports before use, keeps the native library loaded while any V3 handle is
alive, copies UTF-8 request strings synchronously, and exposes typed methods
without wiring into AlorEngine.

## Offline acceptance

The native fake-runtime test drives one handle through two serial epochs:
SELL plan/canonical/begin/submit, repeated Working polls with no automatic
cancel, explicit cancel to Cancelled and finish; then a fresh BUY plan with a
new SHA, rejection of the old canonical, another explicit cancel and finish.
It checks one Add and one cancel per epoch, fixed quantity, wrong SHA and
modified/request-mismatched canonical refusal, active-epoch refusal, and
publisher counters. A crossed-book plan remains closed before publisher
allocation. An active restart checkpoint blocks planning/new begin while
reconciliation remains callable through V3. A separately created terminal
journal is then recovered through V3 with a deliberately crossed current
book, zero additional publisher calls, released locks, and an explicit
reconciliation result.

The managed smoke independently validates all layouts, native-library lifetime
protection, the same two-epoch sequence, exact byte plans, host-managed
identifier advancement, fixed quantity, and explicit stop/dispose using the
same fake runtime. Existing V1/V2, moexctl, ConnectorHost, and lifecycle tests
remain in the repository test suite. No live T1 connection, publisher message,
or order is used by this PR.

## Validation

- Full CTest with the repository Python environment: 166/166 passed.
- Changed-target ASan/UBSan: `connector_host_test`, V2, and V3 passed 3/3.
- Native source-style and `git diff --check`: passed.
- Managed V3 layout/lifetime and two-epoch smoke: passed.

## Stop gate

V3 is a translation boundary only. This PR does not merge automatically and
does not begin AlorEngine integration; the next application increment is a
separate change after review.
