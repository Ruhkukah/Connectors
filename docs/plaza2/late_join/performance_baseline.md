# Bounded AGGR projector baseline

2026-09-18. Real `Plaza2Aggr20BookProjector`, synthetic decoded-row replay,
local Darwin arm64 Release build. The benchmark completed in **0.902 seconds**
including seeding and warmup, with a 45-second process alarm. Product code was
not changed for this measurement. The JSON records exact harness, binary,
linked archive and current source hashes; the shared checkout is dirty, so the
base Git SHA alone does not identify the measured build.

| Instruments / rows | Updated slot | Median / p95 microseconds | Commits/s | New calls / requested bytes per commit |
| --- | --- | --- | --- | --- |
| 1 / 40 | selected, first | 2.792 / 2.833 | 358,305 | 3 / 4,544 |
| 100 / 4,000 | selected, first | 22.375 / 28.583 | 42,815 | 102 / 8,504 |
| 100 / 4,000 | unrelated, last | 24.625 / 27.250 | 39,584 | 102 / 8,504 |
| 1,000 / 40,000 | selected, first | 326.042 / 342.917 | 3,066 | 1,002 / 44,504 |
| 1,000 / 40,000 | unrelated, last | 351.125 / 373.292 | 2,830 | 1,002 / 44,504 |

Each instrument has 20 bids and 20 asks. Each measured transaction updates
one existing replication slot by increasing its revision. Each case has 20
warmup and 500 measured commits. Throughput is the reciprocal of mean timed
transaction latency, excluding seeding and target snapshot reads. There is no
unrelated case at one instrument. First versus last slot deliberately probes
the best/worst search positions; the difference is not an intrinsic property
of whether an instrument is selected.

The independently timed `snapshot_for_isin(1)` copy averaged 0.087–0.097
microseconds across these cases, with one 4,480-byte allocation per copy.
Unrelated commits preserved the selected instrument's hash, version and commit
timestamp. Selected commits advanced its version. All projector calls and
retained row-count checks passed.

## Why the cost grows

Source inspection of `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`:

- `commit()` near line 835 linearly searches all retained rows by `repl_id`
  for each staged row. The unrelated case searches the last slot.
- Near line 872 it copies the previous affected instrument snapshot, clears
  its copied level vector, then scans the entire retained book to refill it.
  Sorting and hashing are target-scoped, but finding those 40 rows is not.
- Near line 924 it unconditionally scans every retained row to rebuild global
  counts, maxima and best prices. The temporary `std::set` allocates one node
  per instrument, even with no qualification observer attached.
- `on_row()` inserts a touched-instrument set node. Along with the 40-level
  snapshot copy and global set, this explains the measured `I + 2` ordinary
  allocations per steady-state single-instrument commit. Short strings stay
  inline in this fixture; long strings could cost more.

For N retained rows, B staged rows and A touched instruments, this path includes
O(B*N) slot searches, O(A*N) target reconstruction scans and a global scan with
set insertion work. These measurements use B=A=1. Bulk seeding is excluded from
the latency table and should not be interpreted as measured startup throughput.

## Concrete next optimization

Introduce a replication-slot index (`repl_id` to row) and per-instrument row
ownership so a transaction rebuilds only its affected instruments. Preserve
both old and new instrument ownership when a mutable replication slot changes
ISIN. Maintain instrument cardinality incrementally instead of rebuilding the
temporary set. Move global diagnostic reconstruction to an explicit diagnostic
read or a cache invalidated by commits, preserving the public snapshot and
qualification-observer contract. The goal is to remove full-book work from
ordinary single-instrument commits, not merely accelerate the target lookup.

Before adopting that change, compare canonical hashes and ordered levels over
insert/update/delete, price/side/ISIN migration, empty books, signed/zero prices,
rollback and invalidation. Preserve untouched target version/time semantics and
commit atomicity. Then rerun the same matrix and add multi-row bursts and churn.
No optimization has been implemented in this sidecar.

## Measurement limits

This is one short, unpinned run on a shared development machine. CPU-model
inspection was sandbox-denied. Timing includes the real begin/row/commit path,
allocation counters and clock sampling; the input bypasses runtime BCD decoding
using already-decoded exact scale-5 values with matching decimal text. Allocation
counts cover ordinary scalar/array C++ `new`, not C `malloc`, aligned allocations,
resident memory or peak retained memory.

There is no CGate network/callback path, authority state, ConnectorHost/DTC
transport, observer, persistence or downstream pressure in the timed region.
Fixed-depth short-string revision updates do not represent all exchange traffic.
The result demonstrates algorithmic scaling; it does not establish live feed
capacity, production safety, Linux VPS throughput or end-to-end latency.

The temporary harness and binary remain outside the repository at
`/private/tmp/moex-aggr-perf.T9pvgK/`; that path is ephemeral. Only this compact
analysis and the machine-readable result are repository artifacts. No raw
logs, executable binaries or source archives are included.
