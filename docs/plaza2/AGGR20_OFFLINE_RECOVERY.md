# AGGR20 lifecycle recovery

The standalone market-data runner and TEST trading host now share one AGGR20 listener bridge. This removes their differing
LifeNum/CLOSE behavior and avoids a second recovery engine. Both discard visible/staged levels and readiness on listener loss,
LifeNum, stop or callback failure. The standalone runner no longer exposes stale levels after stop/failure or remains labeled
Ready after losing its valid snapshot.

CLOSED/ERROR listeners are supervised with a one-second retry delay. Reopen uses a fresh `mode=snapshot+online` bootstrap,
then requires transaction commits, ONLINE and nonempty fresh data before readiness. It does not reuse a replication token after
clearing the projection. The standalone runner exposes Recovering while the fresh snapshot is pending. A LifeNum change waits
for CGate retransmission and ONLINE; it does not fabricate a completed snapshot from retained levels.

ClearDeleted still uses conservative full invalidation, but now explicitly requests a fresh snapshot instead of remaining stuck
with a cleared book and no path back to readiness. This does **not** implement the more efficient table-revision selective purge.
The authoritative older-than-boundary rule is documented in the public recovery contract; selective AGGR compaction remains
an optimization requiring transaction-order fixtures. No reconstructed anonymous L3 logic is introduced.

The shared connection lifecycle and private TRADE/POS recovery policy are unchanged. This patch recovers the AGGR listener on
an existing usable connection; it does not add private-stream mid-run reconstruction or automatic execution re-arming.

Deterministic fake-runtime scenarios cover CLOSE, ERROR without a close callback, LifeNum, ClearDeleted, the 999/1000 ms retry
boundary, fresh snapshot restoration, and visible-book invalidation on stop. No T1 access or exchange orders are involved.
