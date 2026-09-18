# Full trading-day TEST observer

This standalone listener-only app uses the same `Plaza2Env`, `Plaza2Connection`,
and `Plaza2Listener` wrappers as the authority probe. It creates no publisher,
commands, orders, private-state projector, or order profile. Only the four
compiled public services are possible: AGGR20, REFDATA, SESSIONSTATE, and
INSTRUMENTSTATE. The endpoint is fixed to `127.0.0.1:4101`. Runtime, scheme and
T1 config hashes are pinned to the validated September 18 evidence runner.
SESSIONSTATE and INSTRUMENTSTATE use negotiated schemes, as in the existing
connectivity qualification; REFDATA and AGGR use `REFDATA` and `Aggr` selectors.

## Build and offline validation

```sh
cmake -S . -B /private/tmp/moex-day-observer-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMOEX_BUILD_DOTNET_TESTS=OFF
cmake --build /private/tmp/moex-day-observer-build --target moex_plaza2_day_observer -j4
ctest --test-dir /private/tmp/moex-day-observer-build -R '^plaza2_day_observer_fixture$' --output-on-failure
```

Build a Linux executable directly on the evidence host using
`/home/azgaldov/.local/bin/cmake` and `g++` (no Docker required), with the same
configure/build/test commands and a separate absolute Linux build directory.
A macOS executable is only suitable for local offline validation.
Record the source base SHA, dirty source diff/file hashes and Linux binary hash
alongside the external deployment package. This change intentionally makes no
commit and does not label an uncommitted build as the PR head alone.

## Deployment recipe (prepared only; not executed)

The reference environment is the main checkout's
`.codex-tmp/aggr20-authority-probe-d16-5-20260918T0900MSK/operator-runner.sh`.
The wrapper uses its exact library, router, config, scheme and secret-file paths;
it does not source or run that old operator script. Its existing sealed evidence
and executable remain untouched. Transfer the new Linux executable and this
wrapper to a new qualification directory for the launch.

On the evidence host, run the following with actual paths and the newly built
Linux binary's SHA-256; the evidence directory must not already exist:

```sh
systemd-run --user --unit=moex-read-only-day-observer --collect \
  --setenv=MOEX_OBSERVER_SOURCE_SHA=FULL_40_HEX_REVIEWED_SOURCE_SHA \
  /bin/bash /ABS/NEW/plaza2_day_observer.sh \
  /ABS/NEW/moex_plaza2_day_observer NEW_BINARY_SHA256 \
  /home/azgaldov/moex/qualification/NEW_DAY_OBSERVER_EVIDENCE 86400
```

Use the host's existing persistent service mechanism if its user manager is not
enabled. The wrapper checks binary/runtime/config hashes, exact router process,
its executable and native log, and ownership of the listening port before
launching. It neither starts nor restarts the router. A changed config or runtime
requires an explicit reviewed pin update, never bypassing the checks. Start
before the relevant T1 session boundary; 86400 seconds spans a complete day
including the following day's boundary when started mid-session. Maximum is
seven days. No stop occurs when session_data_ready first appears.

## Evidence contract

`events.jsonl` is append-only, exclusively created, mode 0600; each complete
record is synchronously written and fsynced. The directory entry is fsynced at
creation. Memory is bounded by one decoded row and four listener states; there
is no accumulating event vector or in-memory identity/book projection. Disk
usage grows with evidence; disk-full/write/sync failures are fatal and must
never be interpreted as a complete day. Watch free disk and callback latency
on the target host; full-day vendor throughput is not established offline.

Every AGGR sys_event and every decoded REFDATA/SESSIONSTATE/INSTRUMENTSTATE row
is recorded, retaining named fields, raw bytes, replication metadata, receive
wall/monotonic time, server_time where supplied, transaction ID, row ordinal,
listener generation, and before/after ONLINE provenance. AGGR depth rows are
excluded. Book-only AGGR begin/commit pairs perform no evidence writes; one
bounded begin record is deferred until the first sys_event, preserving its
original receive timestamps. Transaction IDs, row indices and source-row counts
include omitted book traffic. Journal sequence is write order; deferred begin
timestamps can precede records already written from another stream.
Generated metadata calls the AGGR stream `FORTS_AGGR##_REPL`; the wire
subscription is `FORTS_AGGR20_REPL`.

Committed membership is a journal relation: join each row to the later
`transaction_commit` by `(stream, generation, transaction_id)` with
`transaction_committed=true`. No matching marker, or a false marker, means the
row must not count as a committed witness. A partial final line after a crash
is discarded. LifeNum, Close and ClearDeleted invalidate an outstanding
transaction. ClearDeleted includes table_code, revision and flags; it does not
erase earlier forensic records. Do not infer exchange order across streams from
equal receive timestamps. Fresh snapshot+online is used for every generation;
no persisted replstate is replayed as current authority.

Ordinary listener close/error and changed LifeNum trigger fresh subscriptions
after a bounded five-second retry interval, until the deadline or SIGTERM/INT.
Each recovery records a gap and generation boundary. Initial LifeNum is normal.
Permanent callback/decode/schema failures are terminal. REFDATA membership,
session identity and current status remain independent raw evidence; this app
does not infer tradability or confer order authority. ONLINE is recorded as a
transport/snapshot boundary, not as a session_data_ready witness.

The shared runtime explicitly decodes CP1251 into UTF-8 once. The journal uses
`text::json_escape_utf8` on the already decoded strings, with no second codec.
Raw field/payload hex preserves original bytes. Malformed UTF-8 is escaped as
U+FFFD. ASCII identifiers remain unchanged after JSON parsing.
The offline fixture includes the exact CP1251 bytes for
`Фьючерсный контракт ALRS-12.26`. No DTC/UI code is modified here.

This observer intentionally does not call `late_join_display_corroborated`:
that helper borrows committed host projectors, authority and transport state
that this listener-only journal does not own. All four streams' transaction
membership, exact identity/status fields and fresh-generation boundaries are
preserved for offline corroboration replay. It must not report a live
corroboration PASS merely from ONLINE or a historical ready event. The live
changed-hypothesis verdict belongs to the host/probe worker consuming the helper.

Heartbeat records every 30 seconds show whether all four streams are ONLINE.
Exit 0 means deadline reached with all streams ONLINE at the end and no recorded
recovery gap, not a certification result. Exit 2 means interrupted, recovered
with gaps, or incomplete streams; 1 means a fatal failure. A missing final `end`
record also means incomplete capture. SIGKILL cannot write a final record.
The required `MOEX_OBSERVER_SOURCE_SHA` must be the reviewed source identity
(40 hex digits); it is recorded in provenance. For an uncommitted build, preserve
its diff and new-file hashes externally as described above, not just the base SHA.
The wrapper retains stdout/stderr, exit status, provenance and SHA256SUMS,
verifies the manifest into `SHA256SUMS.verify`, then seals the exact newly created
evidence directory with `chmod -R a-w`. It records native log identity without
copying the full historical router log.
Full journals/binaries stay in the external evidence store, never the source PR.
