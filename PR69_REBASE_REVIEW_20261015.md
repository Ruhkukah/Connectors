# PR69 rebase and review, 15 October 2026

PR68 base: `3c52504744fd1ac2c47b0dca3838f1e82b0bb538`. Rebased code: `3871650d6c6ef824a03d68407fe75e5766c1ed09`. Previous PR69: `0434c1b08a912c620c0dcdde793a9bc3b5719d3e`. PR68 remains unchanged. This review concerns the Full Order Log branch only.

## Review boundary

All shared-code changes below are carried from the existing PR69 and remain pending Claude/owner review. No new shared semantic change was required or implemented by this rebase. Nothing is merged or deployed by this report. The Full Order Log remains outside this certification round.

The independent audit verified all 19 added production/test/tool files byte-for-byte against the previous PR69 and the same added/deleted code sequences in all 17 shared C++ files. PR68 anchored private TRADE replay, publisher prewarm/admission, native pump/diagnostics, instrument-session binding, indexed lookups and optimized ClearDeleted behavior remain intact.

Text conflicts were combined in four files: cgate_session.cpp retained AnchoredTradeEvents alongside the existing FullLog matching/cache hooks; ConnectorHost CMake retained Threads::Threads and the FullLog library; fake runtime retained revision-floor parsing and FullLog open bookkeeping; CERTIFICATION retained all current PR68 sections. The original PR69 duplicate FullLogRefdata collector had already been replaced by the shared committed projection; that existing replacement is retained.

Production footprint against the current PR68 base: 1,500 gross added C++ lines (991 in six new files, 509 in 17 shared files); eight CMake additions are counted separately. The 45 code/test/build/document paths below are the rebased implementation diff. This review document is additional documentation.

## Order-log production files

| File | Added | Removed | Change |
|---|---:|---:|---|
| `apps/full_order_log_dtc_loop.hpp` | 94 | 0 | Shared actual runner/benchmark owner loop with cached metadata and committed DTC pumping. |
| `apps/moex_full_order_log_dtc_runner.cpp` | 166 | 0 | Standalone read-only process, separate router/instance guards and rate/lag counters. |
| `connectors/connector_host/include/moex/connector_host/full_order_log_dtc.hpp` | 39 | 0 | FullOrderLog source interface. |
| `connectors/connector_host/src/full_order_log_dtc.cpp` | 107 | 0 | Committed price-level snapshots, increments and authority states. |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_full_order_log.hpp` | 58 | 0 | Bounded book and lifecycle interface. |
| `protocols/plaza2_cgate/src/plaza2_full_order_log.cpp` | 527 | 0 | Native schema binding, exact prices, add/cancel/trade, clears and lifecycle handling. |

## Shared production changes: pending review

| File | Added | Removed | Change |
|---|---:|---:|---|
| `connectors/connector_host/include/moex/connector_host/connector_host.hpp` | 1 | 0 | Committed market-data metadata revision accessor. |
| `connectors/connector_host/src/connector_host.cpp` | 8 | 3 | FullLog metadata transport/currentness and accessor wiring. |
| `connectors/connector_host/include/moex/connector_host/dtc_market_data.hpp` | 18 | 3 | FullLog source, book state, commit and status fields. |
| `connectors/connector_host/include/moex/connector_host/dtc_read_only_server.hpp` | 8 | 1 | FullLog configuration and empty/source-withdrawal interface. |
| `connectors/connector_host/src/dtc_read_only_server.cpp` | 140 | 51 | Anonymous positional updates, empty completion, source authority/withdrawal and bounded queues. |
| `connectors/connector_host/include/moex/connector_host/operator_config.hpp` | 2 | 1 | Router-address parser and optional local-password environment setting. |
| `connectors/connector_host/src/operator_config.cpp` | 26 | 7 | Parse/render optional local_pass and expose the existing router parser. |
| `connectors/plaza2_trade/include/moex/plaza2_trade/cgate_session.hpp` | 6 | 1 | Opt-in composite handler/configuration, health and matching/metadata accessors. |
| `connectors/plaza2_trade/src/cgate_session.cpp` | 130 | 3 | Opt-in matching selection/cache, composite creation/recovery and 30-second fresh-snapshot watchdog. |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_metadata.hpp` | 1 | 1 | Optional matching descriptor metadata. |
| `protocols/plaza2_cgate/src/plaza2_metadata.cpp` | 13 | 1 | Optional instr2matching_map field descriptors. |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_private_state.hpp` | 3 | 0 | Optional matching snapshots and committed metadata/refdata revision tokens. |
| `protocols/plaza2_cgate/src/plaza2_private_state.cpp` | 64 | 14 | Opt-in matching rows and staged operations; cached committed joins/tokens. |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_private_state_bridge.hpp` | 6 | 0 | Optional required FullLog REFDATA validation interface. |
| `protocols/plaza2_cgate/src/plaza2_private_state_bridge.cpp` | 22 | 0 | Validate required FullLog matching/contract fields only for that profile. |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_runtime.hpp` | 27 | 2 | Borrowed raw field/table descriptors and opt-in raw/log/snapshot callbacks. |
| `protocols/plaza2_cgate/src/plaza2_runtime.cpp` | 34 | 16 | Negotiated descriptor validation, bounded raw payload delivery and event hooks. |

## Build wiring

| File | Added | Removed | Change |
|---|---:|---:|---|
| `apps/CMakeLists.txt` | 2 | 0 | Add FullLog targets; retain all PR68 targets, Threads linkage and test registrations. |
| `connectors/connector_host/CMakeLists.txt` | 2 | 1 | Add FullLog targets; retain all PR68 targets, Threads linkage and test registrations. |
| `protocols/plaza2_cgate/CMakeLists.txt` | 4 | 0 | Add FullLog targets; retain all PR68 targets, Threads linkage and test registrations. |
| `tests/CMakeLists.txt` | 34 | 0 | Add FullLog targets; retain all PR68 targets, Threads linkage and test registrations. |

## Tests and diagnostic tools

| File | Added | Removed | Change |
|---|---:|---:|---|
| `tests/connector_host_dtc_server_test.cpp` | 215 | 2 | Extend shared offline fixture/test coverage; baseline assertions must also pass unchanged in a separate validation tree. |
| `tests/connector_host_test.cpp` | 31 | 0 | Extend shared offline fixture/test coverage; baseline assertions must also pass unchanged in a separate validation tree. |
| `tests/full_order_log_dtc_source_test.cpp` | 141 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |
| `tests/full_order_log_isolation_test.cpp` | 339 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |
| `tests/full_order_log_live_metrics_test.py` | 39 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |
| `tests/full_order_log_runner_test_driver.cpp` | 15 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |
| `tests/plaza2_cgate/fake_cgate_control.hpp` | 17 | 1 | Extend shared offline fixture/test coverage; baseline assertions must also pass unchanged in a separate validation tree. |
| `tests/plaza2_cgate/fake_cgate_runtime.cpp` | 136 | 6 | Extend shared offline fixture/test coverage; baseline assertions must also pass unchanged in a separate validation tree. |
| `tests/plaza2_cgate/full_order_log_live_metrics.py` | 91 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |
| `tests/plaza2_cgate/full_order_log_live_probe.cpp` | 217 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |
| `tests/plaza2_cgate/full_order_log_live_probe.hpp` | 397 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |
| `tests/plaza2_cgate/full_order_log_live_probe_test.cpp` | 215 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |
| `tests/plaza2_cgate/full_order_log_replay.hpp` | 150 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |
| `tests/plaza2_cgate/full_order_log_test_support.hpp` | 166 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |
| `tests/plaza2_cgate/plaza2_full_order_log_benchmark.cpp` | 343 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |
| `tests/plaza2_cgate/plaza2_full_order_log_session_test.cpp` | 270 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |
| `tests/plaza2_cgate/plaza2_full_order_log_test.cpp` | 431 | 0 | FullLog offline regression, replay/benchmark, or read-only diagnostic capture tooling. |

## Certification documentation

| File | Added | Removed | Change |
|---|---:|---:|---|
| `CERTIFICATION.md` | 18 | 0 | Separate login/router, positional contract, conditional Kairos status and unqualified live-observation plan; existing PR68 sections retained. |

## Validation and benchmark

Fresh local Release, ASan/UBSan and unchanged PR68 baseline validation are recorded in the PR summary after completion. The baseline validation tree uses rebased production with every tracked PR68 test and rehearsal script copied byte-for-byte from the exact base. No PR68 checkout, certified binary, TEST deployment or service is changed.

The benchmark uses the actual ConnectorHost/FullOrderLogDtcLoop and a real loopback DTC drain: 5,000 REFDATA rows, 100,000 retained orders, one million input rows, 90% filtered ISINs, admitted adds/cancels/trades in a 40/40/20 mix with level churn. Report input and admitted rows/s separately, callback-to-book p99, commit-to-DTC-queue p99, memory and row allocations. These are offline fake-CGate measurements.

## Router and live observation: not authorized yet

The supplied Full Order Log credentials must remain in a private host secret file or environment, never Git, command output or evidence bundles. A second router is proposed on loopback port4102 in a dedicated root with its own binary, config, auth, logs and PID; the existing port4101 router/config are untouched. Provisioning/start requires separate owner approval and fresh schedule/host-state checks. No host installation, upload or live connection is part of this offline continuation.

Keep order-log processes off TEST during PR68 interruption/certification sessions and do not run on a PR68 session day. Local schedule documents do not establish a future eligible date; the proposed next short session and MOEX certification day are unarmed/unconfirmed.

After authorization and schedule clearance, observe for 60 minutes in the main session. Require snapshot then ONLINE, retain unknown-order/reopen counters, record LifeNum listener/table identity where the native API provides it, every ClearDeleted table/revision and the matching committed info life. Compare each raw ORDBOOK republish only at its exact linked native log life/revision; report any unaligned interior target or administrative fence as incomplete. Capture live msg/s, moment_ns exchange-to-commit lag, CPU and RSS. The diagnostic comparator always leaves P9/overall qualification inconclusive; a native LifeNum with no table identity must be labelled unavailable.

A separate later overlap test needs its own approval and PR68 process/metric provenance. Compare place-to-post p99 against the same workload with order-log running and SIGSTOPped. It is separate from the isolated observation. No order-log certification or formal R7 claim is made.

## Kairos PR111

PR111 remains OPEN and unmerged at `a5bbda87fdf5c1ca339507ac5923ee78eaa3b82d` on the MOEX branch stack, with no attached GitHub checks. Existing evidence records 453 data tests passed (7 ignored), 24 focused DTC tests and a release application check. Those historical tests were not rerun for this Connector-only rebase. Owner approval is required before merge.
