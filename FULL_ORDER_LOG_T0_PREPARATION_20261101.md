# Full Order Log: internal read-only T0 preparation, 1 November 2026

This is a separate sync line from PR69 `00c6f7330c263c2a98201ad35004ff0f9f93ef3f`, merging PR68 `52782266cc9e230dbcbb84bd67433a2febce7d9b` (25 commits after common base `986f1dce`). Merge commit: `4c1b6a0feb2b24cf669d71f200e944bb1077b3ae`. Code/diagnostic snapshot: `ed6531d701ae209342b076e932159efea3435411`. PR68 and PR69 themselves are not changed by this sync branch.

The certified trading build is frozen at source `5330615f7f6c7c632239f6fc6ea820aa6736dcac`, Linux moexctl SHA-256 `8ab263cc48739d4f76861692e849ca49d755297438297040c27497c785651dbb`. It must not be changed, rebuilt in place or redeployed. Only the FOL executable is deployed to a new private SHA-specific release path. Offline fake tests may build their own moexctl under an isolated build root; that artifact is not the certified executable and is not deployed for trading.

## Merge resolution ledger

There are **13 overlapping files, four textual conflict files**. The other nine overlap files merged cleanly; they still required the same preservation audit. No thirteen textual conflicts are claimed.

| File | Merge result | Resolution and retained behavior |
|---|---|---|
| `CERTIFICATION.md` | Clean overlap | Preserve PR68 certification/read-only guidance and retain the existing FOL section; this internal trial grants no trading or certification authority. |
| `apps/CMakeLists.txt` | Text conflict | Keep PR68's read-only qualification resolver target and PR69's separate FOL runner target. |
| `connectors/connector_host/src/connector_host.cpp` | Clean overlap | Preserve PR68 trading/read-only metadata and spread classification. Retain existing FOL source selection; add only a borrowed owner-thread diagnostic getter in the later telemetry commit. |
| `connectors/connector_host/src/operator_config.cpp` | Clean overlap | PR68's server-owned read-only replication settings win: do not force local client schemes. Retain PR69's optional local password. |
| `connectors/plaza2_trade/include/moex/plaza2_trade/cgate_session.hpp` | Clean overlap | Preserve PR68 DEALS target/current-session configuration and existing poll metrics; retain opt-in FOL fields. |
| `connectors/plaza2_trade/src/cgate_session.cpp` | Three textual conflict hunks | Keep PR68 `deals_restart` fencing and both `synchronize_public_deals` calls, anchored private replay, ACTIVE waiting and startup-health logic. Retain the existing FOL matching resolver, schema failure gate and snapshot watchdog. |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_metadata.hpp` | Clean overlap | Keep PR68 Spectra9.12 field count/indices and removal of obsolete opening price; retain the optional FOL matching-descriptor API. |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_private_state.hpp` | Clean overlap | Preserve PR68 future/spread membership semantics and optional opening-price model; retain FOL matching/token declarations. |
| `protocols/plaza2_cgate/src/plaza2_metadata.cpp` | Clean overlap | Preserve PR68 9.12 required fields without opening price; retain optional matching descriptors. |
| `tests/CMakeLists.txt` | Clean overlap | Keep every PR68 registration plus existing FOL tests; append one minute-metrics test. No timeout is relaxed. |
| `tests/connector_host_test.cpp` | Text conflict | Keep PR68 committed outright/spread readiness assertions and retain the independent FOL local-password assertions in a separate scope. |
| `tests/plaza2_cgate/fake_cgate_control.hpp` | Text conflict | Keep PR68 `listener_handles` and PR69 `listener_open_settings` as separate methods. |
| `tests/plaza2_cgate/fake_cgate_runtime.cpp` | Clean overlap | Preserve PR68 DEALS lifecycle/native9.12 fixture support and PR69 FOL replay scripts; do not replace either entire fixture with an older version. |

The merge audit verified all72 PR68-only changed files byte-for-byte and all20 PR69-added files byte-for-byte before the narrowly requested diagnostic changes. All13 overlap deltas relative to PR68 retained the original FOL changes. Trading admission/posting/readiness bodies and PR68 public-DEALS lifecycle bodies were unchanged by conflict resolution. This source audit does not establish live trading equivalence; the frozen certified binary remains separate.

## Scope of the diagnostic follow-up

- The FOL executable rejects `--environment prod` before native startup. Order entry stays disabled: no publisher, reply listener or order APIs are configured.
- A native CLOSE carrying `close_reason=40966` is logged as decimal, `0xA006`, and `REPL:CHANNEL_DISCONNECT_ON_TIMEOUT`. The existing recovery path is retained; no new recovery policy is invented.
- The FOL session regression injects that CLOSE, requires the old book to become unavailable, and requires reopening/fresh snapshot+ONLINE+commit before publication. It also injects40966 as a process error and verifies numeric error logging, recovery, no replay state, no publisher/reply handles and zero fake command posts.
- The FOL bootstrap test uses the actual captured Spectra9.12 REFDATA layout without opening price and holds the connection in OPENING to verify no listener opens or fabricated startup health before ACTIVE.
- The runner enables existing native poll diagnostics only for FOL and reports every60 seconds, plus a final partial window: all native callbacks/s (including unattributed callbacks), separately attributed FOL callbacks, row rate, native process-call count, and `cg_conn_process_loop_max_ns`.
- `cg_conn_process_loop_max_ns` is the maximum **per-owner-poll sum of measured cg_conn_process wall times**, including callback work. It is not the maximum single native call and not the entire owner-poll wall time. Counts/max reset each reporting window; repeated reading of one sample does not double-count it.
- Exchange-to-completed-publication lag continues to use `moment_ns`. `lag_last_ns` and session-cumulative samples/sum/max are labelled as such; a minute report does not make the cumulative maximum a minute-only statistic.
- The DTC listener remains hard-bound to IPv4 loopback (`INADDR_LOOPBACK`), depth20 by default. No DTC execution surface is added.

The full C++/FOL suite, unchanged rehearsal safeguards, Linux CI and separate deployment evidence are reported in the draft PR after completion. Binary hash/source identity and private release paths are recorded in the deployment manifest; they must be checked during Monday preflight. These are offline implementation checks, not a vendor-load or certification claim.

## Manual Monday 2 November sequence

**Each stage waits for a new owner message. No timer, automation, wake-up, service or live process is armed tonight.** This document is not start authority and carries no chosen TERM deadline.

1. **Preflight, no CGate connections.** Verify certified binary/hash unchanged; candidate binary/hash/source manifest; TEST/T0 profile, runtime/library/SDK and private credentials; separate app_name/router4102; loopback DTC ports; no trading process; fresh schedule/owner state. The stopped profile inspected on1Nov points to T1 links. A separate T0 profile/secret selection must be resolved without assuming T0 entitlement. Never use the existing trading router or PROD profile. Report missing inputs; do not connect to infer them.
2. **After a separate start message, after09:30 MSK.** MOEX start-time restriction from the owner applies: never connect before the trading system starts. Confirm the authorized date and exact TERM deadline. Use the same Friday deadline-guard pattern with owned PIDs and immutable candidate identity; retain default vendor logging. Start only the separately approved ORDLOG router/profile, then the read-only FOL process. No trading process may run.
3. **Observation, separately authorized around11:00–12:00 MSK.** Use the existing60-minute plan: REFDATA and composite ORDLOG/ORDBOOK ONLINE, matching ID resolved/logged, publication only after commit, zero0xA006 disconnects, no persistent crossed book, rates/lag/CPU/RSS recorded, DTC depth only on localhost. Raw ORDBOOK comparisons use the exact linked native life/revision; ambiguous/interior targets and P9 remain explicitly incomplete. Optional short14:00 crossing window requires another message; it is not automatically scheduled.
4. **Stop on the owner's message or authorized TERM deadline.** SIGTERM the FOL process first, wait for its exit and verify zero clients on4102, then SIGTERM its own ORDLOG router and verify its exit/zero listeners; preserve logs. The safeguard deadline value is set only when the owner authorizes startup. Never signal the certified trading process or existing trading router. A later window requires fresh permission/preflight. Friday guards pin PID/start ticks/executable/hash, use a lock/idempotent TERM claim, send TERM only and wait at most30 seconds; they never send SIGKILL. Their literal port4101 and Friday deadline values must not be reused for FOL: bind the authorized copies to4102 and the new owner-supplied deadline, and keep the explicit zero-client checkpoint before router TERM.

## 30 October owner-loop question

The original journal is read only. Findings are reported separately in the draft PR/audit artifact with raw UTC/MSK timestamps and evidence lines. Journal gaps or channel-timeout logs alone do not prove a cg_conn_process stall; distinguish observed chronology from attribution and preserve the lack of timing/clock evidence. No change to the frozen trading build follows from this analysis.
