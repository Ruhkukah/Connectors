# PR #63 read-only AGGR20 authority probe — 2026-09-18 09:00 MSK

## Final result

`MIDSESSION_SESSION_DATA_READY_AFTER_ONLINE = NO`

The two attempts completed cleanly and independently:

| Attempt | Opened | ONLINE | Snapshot | Post-ONLINE current-session `event_type=1` | Callback error | Result |
|---|---:|---:|---:|---:|---|---|
| `INITIAL_OPEN_SESSION_DATA_READY_AFTER_ONLINE` | yes | yes | yes | no | none | `NO` |
| `LISTENER_REOPEN_SESSION_DATA_READY_AFTER_ONLINE` | yes | yes | yes | no | none | `NO` |

Because both experiments completed normally, the missing post-ONLINE event
produces `NO`, not `INCONCLUSIVE`. The earlier `session_data_ready` row was
present in both snapshots before ONLINE and therefore does not satisfy the
authority gate.

## Identity

- Connector PR: [#63](https://github.com/Ruhkukah/Connectors/pull/63)
- Candidate source: `2c5464a96f2014b3d53f3f32015a4d4a7dbc47d5`
- Frozen source base: `deff561dd524633268734a4a7eb3dc0763475027`
- Candidate Linux binary SHA-256:
  `279bee891355e4920c4e5531d237604b02752d50477167cd96a2846f16f63e3c`
- Runtime: `SPECTRA9.9.0`
- Runtime library SHA-256:
  `f63e726a8482b793c3af755a8dc2b9ebb5cd727d88fb58ebb3fe9704a155ce6f`
- Scheme SHA-256:
  `7b93117ee435fd0cb2849b677fc32a9d581364b6ee9afeac9c6c002875400746`
- Router PID: `3704670`
- Router native log: `router.t1.4.log`
- Probe interval: `120` seconds per attempt
- Probe exit status: `0`

## Fresh REFDATA selection

- `sess_id`: `11709`
- `isin_id`: `4519450`
- Symbol: `ALRS-12.26`
- Short symbol: `ALZ6`
- Base contract: `ALRS`
- REFDATA LifeNum: `101518051`
- Instrument source: stream `4096455455`, table `1956699083`, replRev `135`
- Session-contents source: stream `4096455455`, table `2984491968`, replRev `1229`
- Session source: stream `4096455455`, table `3627922065`, replRev `3`

All three source rows were present under the same REFDATA LifeNum. ALRS was a
valid current candidate and was selected deterministically.

## Authority evidence

Each attempt recorded 21 committed `sys_events` rows. In both attempts:

- `event_type=1`, event id `678984`, repl id/rev `3/3`, `sess_id=11709`,
  `server_time=1789662416` was committed during the snapshot;
- the row was classified `observed_before_online=true`;
- ONLINE then transitioned authority to `waiting_for_session_data_ready`;
- no current-session `event_type=1` row arrived after ONLINE during the
  120-second observation interval;
- `listener_last_callback_error` was null;
- listener close and destroy completed normally.

The full JSON contains every `sys_events` row, transaction membership,
event identity, repl revision, session id, server time, authority transition,
and event trace for both attempts. The native router log is preserved before
and after the run.

The read-only contract passed:

- publisher create/open: not attempted;
- command API: unused;
- order API: unused;
- authorization hash: unused.

## Evidence

- `evidence/aggr20_authority_probe.json`: complete machine-readable result;
- `evidence/aggr20_authority_probe.log`: result summary;
- `native-router-log-before.txt` and `native-router-log-after.txt`: native log;
- `candidate-provenance.txt`: candidate/runtime/router/config hashes;
- `operator-runner.sh`: exact fail-closed runner used;
- `candidate-source.patch`, `connector-2c5464a.tar.gz`, and
  `candidate-commit.txt`: source review material;
- `SHA256SUMS` and `SHA256SUMS.verify`: immutable remote-root manifest and
  verification output;
- `LOCAL_SHA256SUMS`: local package manifest generated after assembly.

Remote evidence root:

`/home/azgaldov/moex/qualification/authority-probe-d16-5-20260918T0900MSK`

Local review root:

`/private/tmp/aggr20-authority-probe-d16-5-20260918T0900MSK`

No PR description update was made because the required YES/YES condition was
not met. No merge was performed.
