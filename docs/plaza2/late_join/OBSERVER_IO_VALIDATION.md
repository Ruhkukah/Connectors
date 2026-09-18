# Buffered observer durability validation

September 18 correction to the reviewed 06d48e3 baseline; measurement below is
the local macOS Release stress executable, not Linux throughput or a live capture.

The exact Handler/Journal path emitted 50,000 rows in one REFDATA transaction,
retaining a 128-byte raw payload per row, field values and raw P2TIME components.
Observed output: 41,268,248 bytes; 633 write calls; peak write buffer 65,536 bytes;
zero per-row fsync; exactly one commit fsync. Four file syncs total include OPEN,
COMMIT, ONLINE and clean shutdown; directory creation sync is separate.
Measured transaction/shutdown interval 251,871,292 ns, maximum callback 584,042 ns,
maximum write 578,792 ns, maximum fsync 805,916 ns. These are observations on this
filesystem, not real-time upper bounds; synchronous storage can still stall at
a flush or durability boundary. No unbounded transaction queue is allocated.

The independent streaming Python reader validated exactly 50,000 committed rows,
one transaction, no incomplete rows and clean shutdown. Exact file SHA-256:
`f8247b87ed20fc31441b6da3ed190923d6a8bdc433e66fb990c3020f755cccc0`.
The temporary raw benchmark is outside Git, at
`/private/tmp/moex-observer-stress-metrics-20260918.jsonl`.

The CTest stress/fault fixture repeats the clean transaction and abruptly exits
before commit using _exit, bypassing destructors and losing the buffered tail.
The reader reports the transaction incomplete, zero committed rows and no clean
shutdown. A deliberately corrupt commit hash is rejected; a torn final marker
cannot commit its rows. Every emitted identifier, payload byte and P2TIME component
is checked in the clean case. CTest has a 120-second execution limit.

Reader FNV-1a is a transaction consistency check, not a cryptographic signature.
Whole-artifact SHA-256 is retained for forensic integrity. The reader never treats
the mere presence of a row or ONLINE as committed authority. An abrupt-process
fixture is not a physical power-loss test of the disk's fsync implementation.
