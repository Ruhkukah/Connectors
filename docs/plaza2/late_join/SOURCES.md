# Verified source register

Checked 2026-09-18. The protocol basis is the local installed manual titled
`PLAZA II gateway (version 9.9)`, UTF-8 HTML, 1,931,165 bytes:

`/Users/pavel/CSharp/MoexConnector/.codex-tmp/cgate99_installed_p2gate_en.html`

SHA-256: `5e2bfa2f6e8b3bc48f72e2eb46230e61200fde2a4bb5dbf723eabc28f1193c2e`.

Official distribution documentation location:
[MOEX CGate test documentation](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/p2gate_en.html).
Live retrieval of that HTML timed out in this session. Exact anchors below were
verified in the local 9.9 copy, not asserted to exist in today's remote file.
The synchronization discussion is at local HTML lines 2415-2480, with instrument
status documentation immediately following. Anchors and hash are the stable local
citation; line numbers refer only to this exact copy.
The local title confirms the document version; its extraction chain to a particular
vendor package was not independently reconstructed for this packet.

Do not substitute `spec-lock/test/plaza2/manifest.yaml`'s older 9.3 HTML hash.
Runtime identities are independently recorded by
`spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/manifest.yaml` and the sealed probe.

- S1: `Table orders_aggr: Aggregated order-books`, table 46;
  `table_FORTS_AGGR_REPL_orders_aggr`, first Note. Validity requires synchronous
  session_data_ready. Price is d16.5. Zero-volume removal is distinct from zero price.
- S2: `Event-sensitive scheme for data synchronizing`; `idm140692876369696`,
  final three notes. Match events across streams by event_id, not replID/replRev.
  Online events follow corresponding data. Snapshot ordering is not an online
  consistency barrier; a historical row may concern another trading day.
- S3: `Table sys_events: table of events`; `table_FORTS_AGGR_REPL_sys_events`,
  table 47. event_id, sess_id, event_type, message and server_time (`t`);
  event type 1 describes clearing data loaded into trading.
- S4: `Instrument status broadcast service`; `s_2_5_6`. SESSIONSTATE,
  INSTRUMENTSTATE and SECURITYGROUPSTATE provide status; legacy REFDATA statuses
  may be delayed. Status does not document an AGGR synchronization replacement.
- S5: `Table session_state: Current trading day status`;
  `table_FORTS_SESSIONSTATE_REPL_session_state`, table 137. sess_id and public_state;
  states 0 scheduled, 1 running, 2 suspended, 4 completed.
- S6: `Table instrument_state: Statuses of instruments for the current trading day`;
  `table_FORTS_INSTRUMENTSTATE_REPL_instrument_state`, table 139. isin_id and
  public_state, no sess_id. States include auction and close-only restrictions;
  cannot collapse all nonzero states into tradable.

These are paraphrases. None of S1–S6 explicitly confirms the proposed late-start
combination or durable reuse of a previously observed online witness. That gap is
the support question, not a license to call a historical row synchronous.

S7: [MOEX test access and schedule](https://www.moex.com/s438), live page retrieved
2026-09-18; sections `T1`, support-request details and test access registration.
Published T1 boundaries are 07:00, 10:40, 10:59:45, 11:00, 13:00, 14:00, 15:00,
16:00. This is a current schedule reference, not evidence of the exact readiness
publication time. Recheck notices before an observer campaign. The page lists T0/T1;
it does not establish Game access, entitlements or a Game schedule.

S2's general lifecycle examples are not a T1 schedule. In particular, do not infer
T1 session_data_ready time from the manual's approximate production-style times.
