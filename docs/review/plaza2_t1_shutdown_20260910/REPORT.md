# SPECTRA T1 graceful SIGTERM qualification, September 10

**GRACEFUL_SIGTERM_PASS.** One authorized observation, one ordinary SIGTERM, process exit 0.
The separate earlier September 10 observation remains **FAIL / exit 3**. Its seven bound evidence hashes
were rechecked unchanged. No router restart, network fault, order, SIGINT run, performance load test or PR merge.

## Exact live identity and deployment

| Item | Identity |
|---|---|
| Source / PR #50 reviewed head | ca48ae94529f27926c3dffcef9ed68d96a5afaa9 |
| Linux Release binary SHA256 | 482eaf8fb7fd0acfe03ce98ba663569c8c5c0a1af0b1259f0cfbb17ec5248e74 |
| CGate runtime SHA256 | f63e726a8482b793c3af755a8dc2b9ebb5cd727d88fb58ebb3fe9704a155ce6f |
| forts scheme SHA256 | 7b93117ee435fd0cb2849b677fc32a9d581364b6ee9afeac9c6c002875400746 |
| Target | ALRS-9.26 / ALU6, isin_id 4519447, sess_id 11703 |

Copied side-by-side into `/home/azgaldov/moex/connector/aggr-qualification/ca48ae94529f27926c3dffcef9ed68d96a5afaa9`.
The copied binary hash and --version matched. Runtime/scheme/configuration dependencies matched the retained
manifest. Native architecture is x86_64, required GLIBC_2.34 is compatible with host glibc 2.35.
No historical binary, evidence directory or journal was overwritten.

PR47/48/49 retain heads 8c355979c9c24ebbce558c24a1356587ea8a25bd,
f966a7348a3252db98c8e9e363e7b6005e009e99, and e8f4768ced280f937af0665d25496308ec0fb0fc.
The reviewed PR50 head is not amended by this evidence-only branch. All remain draft and unmerged.

Fresh scenario: `/home/azgaldov/moex/evidence/plaza2/2026-09-10/02-shutdown-sigterm/`.
Fresh journal: `/home/azgaldov/moex/journal/plaza2/2026-09-10-shutdown-sigterm/`.
Preflight required absence of order authorization, order.request, stale scenario files and active checkpoint;
Qualify purpose, LiveTestPreSend mode, send arm false. The prior final account census was flat; the new run
then confirmed flatness from fresh private synchronization. Public [MOEX T1 availability](https://www.moex.com/s328)
was available at preflight; fresh runtime synchronization supplied direct connection evidence.

Session 11703 was provisional at launch, then confirmed from the new committed runtime reference row with
exact numeric/symbol identity and matching AGGR target. Initial target replRev was 1829 and final target
provenance replRev 2391, LifeNum 101386965; session provenance advanced 5 -> 6 around the 11:00 transition.
This was a fresh runtime confirmation, not blind reuse of the earlier row. Naturally captured price fields
are retained; the price gate remains MOEX_CLARIFICATION_REQUIRED. No formula investigation/change was made.

## Stable interval and counters

Launch receipt: 10:59:16.975 MSK. Process exit receipt: 11:03:03.688761 MSK.
Stable synchronized duration before SIGTERM: **220.381 seconds** (about 3 minutes 40 seconds).
Total process duration from launch receipt: 226.714 seconds. Normal topology and effective private/AGGR/
publisher/reply readiness remained healthy through the 11:00 transition; composite business readiness
remained false. No attempt was made to relax those gates.

Last recorded one-second metrics sample: 239,993 callbacks; 4,599 AGGR commits; 18,213 polls.
All 225 samples reported zero callback errors, event loss, invalid books, active account orders and unexpected
positions. Final snapshot reports zero publisher message allocations/posts, active order epoch and submission
attempts. Counter values are the last persisted sample, not an invented exact post-teardown total.
The runner's final result also verifies zero accumulated invalid-book, callback-error, lost-event and owner
violation counters; its observation safety invariant checks posts/epochs/submission attempts every loop.

## Signal and shutdown chronology

| Event | Evidence / time (MSK) |
|---|---|
| SIGTERM sent once | 11:03:03.644786447; realtime ns 1789027383644786447 |
| Masks immediately before send | Owner 3644343 and vendor thread 3644346: SigBlk 0x4002 (SIGINT and SIGTERM) |
| Immediately after send | ShdPnd 0x4000 (SIGTERM pending), SigBlk 0x4002; retained external /proc sample |
| Last persisted successful poll sample | poll count 18213, monotonic ns 12069127158710152; approximate wall time 11:03:03.426201 |
| Exact last cg_conn_process begin/end | NOT_INSTRUMENTED in reviewed binary |
| Exact signal-consume / loop-exit timestamp | NOT_INSTRUMENTED; bounded after pending-signal sample and before first teardown action |
| First host.stop vendor action | cg_pub_close at 11:03:03.647329 |
| Publisher/reply ACTIVE -> CLOSED | 11:03:03.647593 / 11:03:03.647499 |
| Replication listener close/destroy | Eight listeners close/destroy between 11:03:03.647688 and 11:03:03.656609 |
| Connection close call | 11:03:03.656778 |
| Connection ACTIVE -> CLOSED / destroy | 11:03:03.657212 / 11:03:03.657242 |
| Environment library shutdown | Replication stopped 11:03:03.657382; MQ stopped 11:03:03.657411 |
| Process wait receipt | Exit 0 at 11:03:03.688761 |

All teardown log entries carry the same owner-thread TID 140390013900608. Exact host.stop entry/return and
individual numeric close/destroy return codes are not separately instrumented. The reviewed runner calls
host.stop on the owner and includes its aggregate success in the final result; the vendor trace records
normal closure, with no cleanup error. Result.json remains the runner's literal **PARTIAL** (host-observation
scope); this separately bound scenario verdict is GRACEFUL_SIGTERM_PASS, not a rewrite of that raw result.

CG_ERR_INTERNAL count: **0**. Interrupted-system-call count: **0**. Fatal ConnectorHost cause: **none**.
Recovery attempts before/after: **0 / 0**, delta 0; recovery generation stays 1 and transitions stay 1.
No synthetic recovery/reconnect is evidenced. The reviewed code consumes pending stops between polls and
exits before another host.poll; there is no per-call live trace to assign a precise last call timestamp.
`state_after.json` is written before host.stop, despite its name, and retains healthy pre-teardown state.

Four vendor ERROR-severity messages during startup are documented field-deprecation notices for version
9.12 (settlement_price_open and ticker). They are retained, not counted as processing or cleanup failures.
There are no other vendor ERROR entries and stderr is empty.

## Private account identity

Classification: **CLIENT_SHAPED_NO_MATCH**.

| repl_id | Structural kind | Length | Equals broker | Equals full client | limits_set |
|---|---|---:|---|---|---:|
| 186438 | Client-shaped | 7 | false | false | 1 |

Private path: `/home/azgaldov/moex/private-evidence/2026-09-10-shutdown/part-identity.json`.
Parent mode 0700; file mode 0600; exclusive new file, no symlink. SHA256:
`7f53171d3c942ed906d72d2a44c89d07d505636dddc9241dbfdeff42b5c2b99b`.
After process exit, a protected local copy was decoded and compared byte-for-byte with both candidates.
The operator-only artifact contains actual code/candidates and their exact comparisons; raw contents are
not committed. Hex is reversible representation, not anonymization. No send gate was changed, and no
additional MOEX support message was sent (the user reports support has already been asked).

## Evidence retention and scope

[Summary](summary.json) and [file hashes](evidence-hashes.json) bind the new result.
Private archive SHA256 `6049fb646924c75bd2627dfc28f304f66e53d388732cf1d1e2d9414d821902cf`,
445,500 bytes. All 23 archived files, totaling 7,356,285 bytes, were verified against their source hashes
in the off-VPS copy. The private archive contains protected configuration/account material and is not
committed. Source evidence remains on the VPS unchanged.

Only graceful SIGTERM shutdown for this exact reviewed source is promoted. Account authorization,
price-limit semantics, router recovery and order lifecycle are not promoted. The earlier source e8f4768
observation and source 96199b6 router-loss results remain historical FAILs.

**Next candidate: ZERO-ORDER CONTROLLED ROUTER RECOVERY**, awaiting separate authorization. No automatic
SIGINT test, router restart, order test, performance run or merge follows this report. Router PID 3576452
remained alive unchanged; exchange orders 0; PR merges 0. Stop here.
