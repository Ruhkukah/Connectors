# PR 68 round-four remediation

This records corrections against Claude's `MOEX_CONNECTOR_PR68_REVIEW_ROUND4_20261002.md`. Local or simulated passes are implementation evidence; vendor CGate and the complete TEST day are reported separately below. D1–D7 remain unchanged, and PR 68 remains a draft.

## Corrections and regression evidence

| Item | Change | Verification |
| --- | --- | --- |
| R1 | After three definitive bulk rejections, queue individual cancellation for each affected nonterminal order, with independent business-rejection budgets. | Regression failed before the fix; checks both sides, instrument scope and each individual's full retry budget. Commit `8d622128`. |
| R2 / R4 private history | Replace the fatal history latch and staged retirement machinery with the existing USERORDERBOOK + TRADE snapshot barrier. Preserve the manager and command correlations; log `private_history_gap`. Keep the 8,192-row delta cap. | Native commit+Close and commit+ClearDeleted tests recover the same logical order, replay the fill once, hold readiness until snapshots complete and resume cancellation/new entries without retrying the original Add. Commit `0056ec8e`. |
| R5 | Retire accepted bulk reply 186's command timer. Await a later TRADE commit without resending, then cancel surviving IDs individually. | Regression failed before the fix; five simulated minutes without TRADE and late timeout callbacks produce one mass-cancel send. Commit `3b48055c`. |
| R6 | Preserve per-order operator alerts across renewed requests, accepted replies and relisting. Clear each alert at terminal proof; derive the aggregate alert from existing affected orders. | Regression failed before the fix; covers independent retry budgets, unrelated terminal orders, partial/final bulk reconciliation and late official identity. Commit `78efffd9`. |
| R7 | Print the shutdown warning only when an order is outstanding. | Flat-host regression failed before the fix; the real storage-failure test still reports the working ID. Commit `d90ebc8c`. |
| R8 | Replace the host-specific template/result/reporter wrapper with plain scope cleanup. | Early returns, exceptions, explicit stop errors and cleanup exceptions retain host stop and outstanding-order reporting. Commit `b952aaf2`. |
| R3 | Remove all eleven provisional Add containers and raw-row replay machinery. Track own rows as ordinary recovered orders; bind the submitted Add only through official 179 and validated relist aliases retained in existing terminal records. | All prior identity, fill, conservative exposure, terminal-pruning and native race regressions pass in Release and ASan/UBSan. The invariant-throwing `pending_.at()` path is removed. Production reduction: 240 lines. |
| R4 late replies | Remove unused command-kind guesses from unknown-UID events. Retain raw UID/message/payload forwarding to the trading owner. | The native late-Move test reproduced a real lost replacement reply after both timers expired without forwarding. The manager owns correlation; standalone unknown replies remain ignored. Commit `fbfa1026`. |
| R4 bulk cancellation | Remove bulk generations, the generation counter and each command's bulk marker. Retire superseded cancellation UID correlations; retain one per-ISIN accepted-reply/TRADE-watermark gate. | An old-UID regression failed before retirement; individual budgets, accepted-186 wait, scope and late replies pass. Commit `cb9dac35`. |
| R4 command scope | Remove Iceberg Add/Del/Move, DelOrdersByBFLimit and CODHeartbeat requests, encoders and reply cases. Keep the four declared commands and system 99/100. | Independent vendor wire fixtures remain unchanged. Excluded replies fail as unsupported; all four declared command encodings pass. Commit `8212924e`. |
| Final R2 safety review | Recheck a queued Move's fill baseline immediately before dispatch. If reconstruction invalidated it, settle the original order and release the queued reservation without sending. | Regression reproduced the rate-held Move sending after reconstruction. The fix also preserves already-posted PossiblySent Move exposure through timeout, late 176 and native terminal proof. Release focused 3/3 and ASan/UBSan focused 1/1 pass. Commit `06c1767a`. |

Snapshot recovery proves current exposure, not complete retired trade history. All previously nonterminal logical orders, including pending Adds, lose their complete-fill-baseline flag on reconstruction. They permit cancellation and refuse Move. Actual execution quantities remain derived only from deduplicated trade records; a fresh order submitted after recovery starts a new tracked baseline.

The production-source count using the audit's tracked `.cpp`/`.hpp` files under `apps`, `connectors` and `protocols` fell from 20,193 to 19,557 lines. All named unnecessary structures were removed, but the result is about 19.6k, above the review's approximately 19.3k target. Test-only fake sources have not been relocated to manipulate that count. The production build compiles 19 units with 13,858 `.cpp` lines; headers are included in the first count.

## Local validation

Implementation head: `06c1767a9e8bef0e0296aecb5ce6f9dd0730d506`. The final full suites below include the queued-Move guard.

- Native macOS Release: all 32 tests passed in one complete final run with local loopback bind permission, 17.01 seconds. Earlier sandbox-bound runs denied bind in two socket tests; both passed when rerun with bind permission.
- AppleClang ASan/UBSan: all 32 tests passed in one complete final run with local loopback bind permission, 22.15 seconds. Darwin LeakSanitizer is unavailable.
- Production-only Release: passed with 19 compilation units and no fake/test/TWIME/ABI sources. No compiler warnings were found in the final build logs.
- Clang-format 18: all 84 changed C++ files passed; whitespace checks passed.
- Defect regressions were exercised against the earlier behavior before correction. The complete compressed `trading_day_test` and order-history reconstruction pass, using the production owner and a controlled fake CGate library.
- The deployment snapshot improvement removes repeated field/snapshot copying while retaining ownership and sorted output. Paired local 150k-row measurements went from 432/423/423 ms to 357/352/365 ms. Online-update p95 remained 1 µs and the first insertion 4–5 µs. The one-second snapshot and one-millisecond update/insertion limits are unchanged.

Published-head Ubuntu Release and ASan/UBSan/LSan status is available from [PR 68's checks](https://github.com/Ruhkukah/Connectors/pull/68/checks). The PR description records the verified CI run. CI is implementation evidence and does not resolve the deployment capacity failure below.

## Deployment and execution evidence

The isolated TEST evidence directory is `/home/azgaldov/moex/qualification/pr68-round4-20261002`. Historical September qualification directories and one-shot markers were preserved. Secrets, account profile values and private raw logs remain on the deployment host.

- Deployment host: Ubuntu 22.04 / Linux 5.15, GCC 11.4, CMake 3.24.3, synchronized NTP and chrony normal.
- Read-only vendor CGate probe: exit 0; committed REFDATA followed by ONLINE. Runtime SHA-256 `f63e726a8482b793c3af755a8dc2b9ebb5cd727d88fb58ebb3fe9704a155ce6f`. No publisher created; zero orders posted.
- Current target: ALRS-12.26 / ALZ6, ISIN 4519450, session 11719, expiry 2026-12-17. Native session timestamps agree with the current [T1 timetable](https://www.moex.com/s438): morning 07:00–10:40, main 11:00–14:00, evening 14:00–16:00, settlement 15:00 and mark-to-market clearing 16:00 MSK.
- Deployment candidate `78efffd9` configured and built in Release. Tests passed 31/32; `plaza2_private_state_transaction_perf_test` failed the unchanged one-second snapshot limit at 1,141 ms. Two repeat measurements, 1,189 and 1,138 ms, also failed. Update p95 was 2 µs; first insertion 8–9 µs. This is a deployment-capacity failure, not a pass inferred from faster local measurements. The final copying improvement in `7d8f738e` still requires an exact-candidate deployment run.
- Automatic approval review blocked transfer of corrected internal sources to this TEST destination pending explicit user authorization. No alternate source-transfer route was used. Final Linux verification and execution await the user's answer.
- The existing TEST profile names an expired instrument and caps orders at one contract. The current instrument is verified above. The partial-fill rehearsal requires an explicit two-contract limit; the configuration question remains pending. No trading scenario is claimed complete from the read-only probe.
- A temporary TEST supervisor is prepared locally with execution disabled. It guards the exact binary/source, current account-flat state, price freshness/current session, order/notional bounds and unique Add labels across restart. Thirteen offline regressions pass, including real local SIGTERM cleanup against a mock child, failed native shutdown, restart/relist ownership and unresolved-ID cancellation. It has not been deployed or used to send commands.

No actual order, router fault, upstream fault or full-day trading scenario has run during this round. Vendor TRADE replay behavior, sustained decode/storage capacity, the complete TEST session and MOEX-coordinated TCS/reload/reserve scenarios remain pending. PR 68 stays a draft; these checks do not authorize PROD operation.
