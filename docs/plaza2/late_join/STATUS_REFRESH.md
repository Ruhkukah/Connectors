# Current-session display and independent status refresh

Online and provisional AGGR witnesses both require the same current identity
gate: healthy completed streams, current REFDATA provenance, one unambiguous
session containing the current wall time, exact selected-instrument membership,
and known current status values. Session end is exclusive. Witness selection
then checks exact session, message, event type, row action and snapshot/online
origin. A synchronous AGGR witness cannot keep an expired session displayable.
Tradability flags are also false when this current identity gate fails.

INSTRUMENTSTATE has no session ID. `InstrumentSnapshot::has_current_status`
remains the raw status presence used by existing private consumers. The new
`current_status_refdata_bound` is separate local freshness evidence: it becomes
true only when an independent status row arrives after current membership
exists. A changed session, removed membership, or REFDATA reset clears that
evidence. Old status is never silently assigned to new session membership.

`Plaza2PrivateStateProjector::status_binding_generation()` advances on committed
membership invalidation or REFDATA generation reset. It is a local counter, not
an exchange session key. Transaction-staged changes publish the counter only
at commit.

`Plaza2TestSessionHost::Impl::refresh_status_after_refdata()` runs on the owner
thread after callback processing. Once REFDATA and the original status
snapshots are complete, it closes both SESSIONSTATE and INSTRUMENTSTATE
listeners, invokes `Plaza2PrivateStateBridge::reset_status_snapshot()`, and
reopens both with `mode=snapshot+online`. No old replstate cursor is used, and
no exchange LifeNum or event is manufactured. The bridge reset clears rows'
status currentness and ONLINE/snapshot completeness before fresh callbacks.

There is one refresh request per committed membership generation. Its marker
is recorded only after both opens succeed and reset when a new bridge run
starts. An open/close failure follows existing transport failure handling;
the existing bootstrap watchdog bounds an incomplete refresh attempt. A
partial optional status-stream profile is left alone. Empty status-stream
configuration retains the pre-existing behavior of installing both defaults.

This handles asynchronous startup: if status arrives before membership, it
cannot corroborate display; after REFDATA completes, the explicit fresh
snapshots supply status independently. It also handles REFDATA-only resets
while status listeners remain ONLINE and send no new rows. Display stays
blocked during refresh. A completed fresh snapshot missing the selected
status remains blocked rather than causing a reopen loop.

Regression coverage in `connector_host_test` includes real projector
membership rollover and immediate/staged REFDATA reset, unordered listener
startup, deliberately stalled refresh, REFDATA-only LifeNum change, exact
listener-open counts, refresh failure, underlying session-host object restart,
partial optional profiles, and online display expiration without an AGGR
disconnect. `ConnectorHost` itself retains its one-shot start contract; the
restart test uses the restartable `Plaza2TestSessionHost`.

All display modes remain read-only. Persisted witness authority is unavailable
until an actual durable write/read/revalidation implementation exists.
