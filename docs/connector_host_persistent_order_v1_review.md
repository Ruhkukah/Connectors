# ConnectorHost persistent TEST order session V1

## Scope and boundary

This document records the native `ConnectorHost` persistent TEST order-session
increment. It keeps one `Plaza2TestSessionHost` and one CGate environment and
connection warm while serial application order epochs are opened, submitted,
observed, explicitly cancelled or filled, and then closed. It does not add
production routing, TWIME, AlorEngine integration, C ABI surface, or a live
TEST order.

The existing `ConnectorHost::authorize()` / `submit()` one-shot `OrderTest`
surface is unchanged. The new native operations are explicit:

```text
begin_order(canonical_plan, sha256)
submit_order()
poll_order()
cancel_current_order()
finish_order_epoch()
```

Every epoch requires a fresh exact canonical plan and full SHA. A stale plan,
wrong SHA, active epoch, or unfinished identifier lock refuses the transition
without allocating or posting a publisher message.

## Serial epoch invariant

At most one non-terminal order epoch is active. `Working`,
`PartiallyFilled`, `CancelPending`, `PossiblySent`, and
`UnresolvedOrphanIncident` keep the epoch active and block a new Add. A poll
never cancels automatically. Only the explicit cancel operation may submit a
DelOrder or the existing bounded exact-`ext_id` recovery operation.

The DelOrder uses the accepted correlated AddOrder reply ID and the existing
participant/instrument checks. A second Add or second cancel in one epoch is
refused. An uncertain or inconsistent epoch cannot be reset by the
application. `finish_order_epoch()` succeeds only after an existing safe
terminal result has been journaled; only then are the order-local intent,
binding, attempt flags, and locks released for the next epoch. The read-side
CGate host remains started throughout.

The supported terminal dispositions remain `Rejected`, `Filled`,
`Cancelled`, and `DefinitelyNotSent`. An orphan incident is fail-closed and
retains the host and identifier locks. The controller exposes typed snapshots
for epoch activity, authorization, Add/cancel replies, lifecycle state,
correlated quantities, market safety, evidence consistency, and whether a new
order is allowed.

## Offline validation

The existing `connector_host_test` now drives two serial order epochs through a
single warm fake CGate session. It proves:

- one environment open and one connection construction for both epochs;
- wrong SHA does not open an epoch or allocate/post a message;
- exactly one Add per epoch;
- a working order remains working across repeated polls with no automatic
  cancel;
- a second Add, a second cancel, and a new order while an epoch is active are
  refused;
- explicit cancellation reaches `Cancelled`, then `finish_order_epoch()` keeps
  the same host usable;
- epoch two requires a fresh plan and rejects epoch-one authorization;
- the second epoch uses fresh order/reply identities and also cancels safely.
- a third fresh epoch reaches `Filled` and closes safely without an automatic
  compensating order.

The existing lifecycle scenario suite continues to cover `Filled`,
`Rejected`, `DefinitelyNotSent`, `PartiallyFilled`, timeout uncertainty,
identity conflict, sticky evidence inconsistency, lock retention, and restart
reconciliation. No C ABI or .NET API was changed.

## Capability statement

The native ConnectorHost/moexctl TEST surface has guarded Add/cancel transport
semantics, but this V1 increment is not a production order router and does not
claim exchange certification. Persistent application trading remains under
development and requires a later separately reviewed integration.
