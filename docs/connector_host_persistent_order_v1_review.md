# ConnectorHost persistent TEST order session V1

## Scope and boundary

This document records the native `ConnectorHost` persistent TEST order-session
increment. It keeps one `Plaza2TestSessionHost` and one CGate environment and
connection warm while serial application order epochs are opened, submitted,
observed, explicitly cancelled or filled, and then closed. It does not add
production routing, TWIME, AlorEngine integration, C ABI surface, or a live
TEST order.

The existing `ConnectorHost::authorize()` / `submit()` one-shot `OrderTest`
surface is unchanged. The new native operations are explicit and take a
per-epoch application request (side, price, base contract, comment, and the
fixed TEST quantity of one):

```text
plan_order(request)
begin_order(request, canonical_plan, sha256)
submit_order()
poll_order()
cancel_current_order()
finish_order_epoch()
```

Every epoch requires a fresh exact canonical plan and full SHA. Collision-prone
ext/user/recovery identities and run IDs are host-managed; the application
cannot supply them. A stale plan, wrong SHA, active epoch, or unfinished
identifier lock refuses the transition without allocating or posting a
publisher message.

## Serial epoch invariant

At most one non-terminal order epoch is active. `Working`,
`PartiallyFilled`, `CancelPending`, `PossiblySent`, and
`UnresolvedOrphanIncident` keep the epoch active and block a new Add. A poll
never cancels automatically. Only the explicit cancel operation may submit a
DelOrder or the existing bounded exact-`ext_id` recovery operation.

The DelOrder uses the accepted correlated AddOrder reply ID and the existing
participant/instrument checks. A second Add or second cancel in one epoch is
refused. After a cancel or exact-ext recovery is requested, subsequent
nonterminal TRADE evidence remains `CancelPending`; a definitive non-timeout
command rejection becomes `UnresolvedOrphanIncident`, while a timeout remains
pending. An uncertain or inconsistent epoch cannot be reset by the
application. `finish_order_epoch()` succeeds only after an existing safe
terminal result has been journaled; it advances the persistent checkpoint and
clears the order-local intent for the next epoch. The terminal journal itself
releases identifier locks, so a crash between those two steps is handled by
the immutable-journal restart path below. The read-side CGate host remains
started throughout.

Before any possible AddOrder publisher call, the host atomically checkpoints
the epoch as `add_may_have_been_sent` under
`<journal_root>/persistent_session.json`. A checkpoint write failure blocks
publisher allocation/posting. Safe terminal completion atomically advances the
checkpoint to `idle` with the next checked identifiers. On restart, an active
checkpoint restores the exact epoch terms and identifiers, blocks planning and
new AddOrder submission, and permits only the existing journal/TRADE
reconciliation path to release locks. An `authorized` checkpoint with no
possible Add is retired without carrying its plan authorization across the
restart. If the process crashes after a safe terminal journal has already
released its locks but before the checkpoint advances, reconciliation validates
that exact immutable journal (including identity, payload, terminal,
consistency, and degradation fields) and advances the checkpoint to `idle`
without rewriting it. No credentials are persisted.

The supported terminal dispositions remain `Rejected`, `Filled`,
`Cancelled`, and `DefinitelyNotSent`. An orphan incident is fail-closed and
retains the host and identifier locks. The controller exposes typed snapshots
for epoch activity, authorization, Add/cancel replies, lifecycle state,
correlated quantities, market safety, evidence consistency, and whether a new
order is allowed.

## Offline validation

The existing `connector_host_test` now drives three serial order epochs through
a single warm fake CGate session and a separate crash/restart fixture. It
proves:

- one environment open and one connection construction for both epochs;
- wrong SHA does not open an epoch or allocate/post a message;
- exactly one Add per epoch;
- a working order remains working across repeated polls with no automatic
  cancel;
- a second Add, a second cancel, and a new order while an epoch is active are
  refused;
- explicit cancellation reaches `Cancelled`, then `finish_order_epoch()` keeps
  the same host usable;
- epoch two requires a fresh plan with different application terms and rejects
  epoch-one authorization;
- the second epoch uses fresh order/reply identities and also cancels safely.
- a third fresh epoch reaches `Filled` and closes safely without an automatic
  compensating order.
- `CancelPending` survives further Working/partial evidence; cancel timeout,
  definitive DelOrder rejection, and definitive exact-ext recovery rejection
  keep locks and block new epochs;
- a host reconstructed from the original base configuration restores the
  unfinished epoch identity, cannot plan/begin/submit another Add, and only
  clears locks after factual terminal TRADE reconciliation; the next epoch uses
  the advanced checked identifiers.
- a crash after factual `Cancelled` terminal journaling but before
  `finish_order_epoch()` is resolved from the no-lock immutable journal,
  advances to the next checked identifiers without publisher allocation, and a
  corrupted terminal marker remains unresolved and blocks a new Add.

The existing lifecycle scenario suite continues to cover `Filled`,
`Rejected`, `DefinitelyNotSent`, `PartiallyFilled`, timeout uncertainty,
identity conflict, sticky evidence inconsistency, lock retention, and restart
reconciliation. Full local validation for this amendment is reported
separately from CI; no C ABI or .NET API was changed.

## Capability statement

The native ConnectorHost/moexctl TEST surface has guarded Add/cancel transport
semantics, but this V1 increment is not a production order router and does not
claim exchange certification. Persistent application trading remains under
development and requires a later separately reviewed integration.
