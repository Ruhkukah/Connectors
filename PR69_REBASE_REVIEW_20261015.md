# PR69 rebase and review, 15 October 2026

PR68 base: `986f1dce89a9471bd0a6a24df7b9500e0bf949af`. Rebased production snapshot: `5f18a5c104df69b0e89a4755f86c6fb7ddd13b4f`. Previous PR69: `0434c1b08a912c620c0dcdde793a9bc3b5719d3e`. PR68 remains unchanged. This review concerns the Full Order Log branch only.

## Review boundary

All shared-code changes below are carried from the existing PR69 and remain pending Claude/owner review. The rebase added no new shared semantics relative to the previous PR69; this does **not** mean the shared changes are behavior-neutral relative to PR68. They alter code compiled into a rebuilt trading binary, including common runtime scheme binding, projector bookkeeping, DTC interfaces and configuration. A separate order-log process does not isolate those source changes from the trading build.

**Certification freeze:** PR69 must not be merged into the PR68/certification build before certification. The certified binary is built from PR68 only. After certification and an approved merge, rerun the PR68 live checks—passive order cycles, actual fills, restart/recovery and shutdown—on the merged binary before it is used for trading. Green offline tests or router provisioning do not release this gate.

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

## No-handler assessment of every shared production file

Baseline B is PR68 `986f1dce89a9471bd0a6a24df7b9500e0bf949af`; reviewed code H is PR69 `e2a6962551f094384b175a37e0eae6b2d95bcdd0`. No-handler means the ordinary legacy configuration: a null `full_order_log_handler`, empty new stream settings and no new CLI option. Null handler alone is not a universal feature switch.

**No PR68 test proves exhaustive neutrality of the merged binary.** The table distinguishes declaration-only source conclusions, conditional legacy functional behavior and concrete unconditional effects. The exact tests below prove only the listed old contracts in their tested fixtures. They do not prove unchanged C++ ABI, instructions, allocations, CPU/RSS, native schema behavior or trading latency. The separate baseline validation used original PR68 fixtures as well as original test sources; ordinary PR69 runs include modified fake fixtures.

| Shared file | Neutral without order-log handler? | Source finding | PR68 test evidence and gaps |
|---|---|---|---|
| `connectors/connector_host/include/moex/connector_host/connector_host.hpp` | **Yes, declaration-only** | One nonvirtual getter declaration; the PImpl object gains no field or vtable slot. This narrow source conclusion does not establish combined-module or binary equivalence. | E1 exercises old host calls; no PR68 test calls the new getter or proves cross-version ABI. |
| `connectors/connector_host/src/connector_host.cpp` | **Conditional legacy result; not exact work neutrality** | Empty full-log stream settings retain AGGR selection/readiness. The selector tests the settings string, not the handler pointer, and adds an emptiness check. Null handler alone is insufficient. | E1 covers ordinary AGGR/current/private readiness; no baseline case for null handler plus nonempty new settings, new getter semantics, or snapshot cost. |
| `connectors/connector_host/include/moex/connector_host/dtc_market_data.hpp` | **No: public C++ ABI/layout changes** | New level/snapshot members and four virtual methods before public_deals affect all providers. Default methods preserve selected source behavior only after rebuilding providers/consumers together. | E2 covers existing Replay-provider defaults and wire identity; no layout/vtable/old-object compatibility, new equality-field or call-cost test. |
| `connectors/connector_host/include/moex/connector_host/dtc_read_only_server.hpp` | **Yes, declaration/default-only in this file** | Adds nonvirtual methods to the PImpl facade; existing queue/depth defaults remain 65536 bytes/20 levels. The changed source interface and implementation are separate findings. | E2 constructs the default server and tests small-queue fencing; neither new API nor cross-version class ABI is tested. |
| `connectors/connector_host/src/dtc_read_only_server.cpp` | **No** | For all sources, accepted ceilings expand from 1MiB/20 to 16MiB/20000; source-row bound becomes 2*configured depth; subscriptions add configure_depth_limit and a second snapshot call. Shared revoke also clears trade subscription/output. Full-log payload selection uses source-kind, not the handler pointer. | E2/E3 cover default AGGR wire, identity, revoke/reconnect, queue fencing and trade/depth independence. No PR68 case proves old upper-bound rejection, reduced-depth/larger-source trimming, snapshot call-count/consistency, partial queued-trade accounting, or timing neutrality. A source-level counterexample is depth1 with three valid source rows: formerly trimmed, now rejected before trimming; not executed by the baseline suite. |
| `connectors/connector_host/include/moex/connector_host/operator_config.hpp` | **No: public aggregate layout changes** | Adds local_pass_env_var string to the public input structure and exposes router_address. Empty new option preserves selected rebuilt legacy calls, not old compiled aggregate layout. | E1/E4 cover old config and instance IDs; no aggregate ABI or new password-field assertion. |
| `connectors/connector_host/src/operator_config.cpp` | **No globally; conditional for unchanged legacy invocation** | All modes recognize --local-pass-env and new help text. Omitted option skips new environment/auth logic; the moved router parser retains its body. | E1/E4/E5 cover ordinary config, default/explicit instance IDs and ext_id ownership. No baseline test supplies the new option or proves its validation/redaction, old unknown-option/help behavior, startup cost or ABI equivalence. |
| `connectors/plaza2_trade/include/moex/plaza2_trade/cgate_session.hpp` | **No: public aggregate layouts change** | Null handler and empty stream are defaults, but configuration and transport-health members/layouts change in every build. Existing all_active predicate is retained. | E6 constructs default trading configs and exercises health/recovery; no sizeof/offset/old-object ABI test or exhaustive pointer/settings matrix. |
| `connectors/plaza2_trade/src/cgate_session.cpp` | **Conditional functional path; no exact neutrality** | Null handler makes the resolver return before joins/listener creation. Ordinary stream codes retain their retry path. Every session still calls the false bridge setter, each active poll calls the resolver, and listener checks/timer storage and stop resets change. | E6/E7/E8 cover default trading/replay/recovery, bounded owner polling, command timing instrumentation and committed readiness. No direct assertion of zero composite allocations, identical memory/instructions/p99, or all new pointer/settings combinations. |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_metadata.hpp` | **No: C++ function ABI changes** | FieldsForTable(TableCode) becomes FieldsForTable(TableCode,bool=false). Default arguments preserve source calls after recompilation, not the old symbol/signature; no compatibility overload is retained. | E9/E10 test default descriptor/schema behavior after recompilation, not old-object/new-library linkage. |
| `protocols/plaza2_cgate/src/plaza2_metadata.cpp` | **Conditional default descriptor result** | With include_matching=false the old descriptor span is returned. A new condition, static optional descriptor array and changed function signature still enter the binary. Whole-runtime OPEN behavior is assessed separately. | E9/E10 cover generated names/types and unused matching fields remaining optional. No new true-branch, byte-for-byte baseline/candidate runtime comparison, or latency/ABI proof. |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_private_state.hpp` | **Yes, declaration-only** | Adds three nonvirtual methods; the public projector remains a single PImpl pointer. This does not neutralize its changed implementation. | E11/E12 exercise existing visibility/default exclusion; no baseline assertion uses the added methods. |
| `protocols/plaza2_cgate/src/plaza2_private_state.cpp` | **No** | Matching projection remains opt-in false, but new counters/state and metadata_changed calls are shared. The helper runs after every commit; counters advance for REFDATA, status and no-stream domains, not normal tagged TRADE commits. Lifecycle/reset calls also advance relevant tokens. | E12/E13/E14 cover default matching exclusion, visibility/clear/lifecycle and selected direct-bridge performance. No baseline test references new revision tokens, enabled matching/replID behavior, added bookkeeping cost or equivalence of allocations/latency. |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_private_state_bridge.hpp` | **Conditional legacy field selection; C++ layout changes** | The new flag defaults false and returns legacy descriptors, but a member and virtual override are added. All dependent C++ units require a full rebuild. | E14 exercises the ordinary bridge/default exclusion; no new descriptor-hook or object-layout equivalence assertion. |
| `protocols/plaza2_cgate/src/plaza2_private_state_bridge.cpp` | **Conditional legacy projection** | Required matching table/field checks are gated by the false default. The session setter and extra dispatch/flag checks still execute on the shared path. | E14 covers owned callback values, commit barriers, default ignored matching rows and clears. No PR68 test directly exercises require_full_order_log_refdata(false/true), new required fields, or zero overhead. |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_runtime.hpp` | **No: internal C++ event/vtable/API changes** | Adds raw table/field references to events, changes wants_raw_replication signature and adds virtual hooks for all handlers. Vendor CGate C ABI structs are not changed by this diff. | E15/E10 cover compiled legacy handlers and selected decoded/schema cases. Existing vendor C-struct ABI assertions do not prove equivalence of this internal event layout or handler vtable. |
| `protocols/plaza2_cgate/src/plaza2_runtime.cpp` | **No** | On legacy replication OPEN, descriptor strings/vectors are built for every recognized native table/field before raw gating; unknown tables remain skipped. Shared required-field row-bound rejection is stricter. Default virtual field/log hooks and raw-table pointers also change ordinary event work. | E15/E10 cover retained decoded values, timing and missing/retyped required versus unused fields. No PR68 test proves new binding-plan equivalence, descriptor allocations/memory/OPEN latency, general row-bound behavior or new pointer/hook semantics. |

### Exact unchanged PR68 test assertions

| ID | Existing CTest | Verified baseline function/assertion |
|---|---|---|
| E1 | `connector_host_test` | [main/config:207–212](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/connector_host_test.cpp#L207) checks router/instruments/session defaults; [224–232](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/connector_host_test.cpp#L224) checks old readiness, AGGR and quiet-poll version; [249–255](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/connector_host_test.cpp#L249) preserves private readiness independent of AGGR; [341–347](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/connector_host_test.cpp#L341) checks read-only handles/authority. |
| E2 | `connector_host_dtc_server_test` | [replay_roundtrip:377–416](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/connector_host_dtc_server_test.cpp#L377) asserts exact AGGR authority JSON, row/revision identity and version refresh; [revoke_and_reconnect:602–617](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/connector_host_dtc_server_test.cpp#L602) covers withdrawal/fresh snapshot; [rejects:843–848](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/connector_host_dtc_server_test.cpp#L843) requires AGGR row identity; [bounds:894–912](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/connector_host_dtc_server_test.cpp#L894) checks old empty-book and small-queue rejection. |
| E3 | `connector_host_dtc_public_deals_test` | [test_trade_only_revalidates_definition_and_keeps_depth_independent:579–612](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/connector_host_dtc_public_deals_test.cpp#L579) checks trade retirement/depth independence. |
| E4 | `dtc_runner_receipt_json` | [runner_instance_regression:49–62](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/dtc_runner_receipt_test.cpp#L49) asserts default/explicit instance names, separate DTC instance and no execution surface. |
| E5 | `moexctl_command_channel_test` | [explicit login and instance ext_id scope:631–643](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/moexctl_command_channel_test.cpp#L631) checks assigned ext_id on fake Add and visible working exposure at forced shutdown. |
| E6 | `plaza2_session_recovery_test` | [main:705–718](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_session_recovery_test.cpp#L705) asserts nonblocking OPENING/recovery; [734–767](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_session_recovery_test.cpp#L734) covers private ONLINE/Add, CLOSED refusal and fresh snapshots; [779–789](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_session_recovery_test.cpp#L779) bounds process calls/owner yield and permits cancel; [822–850](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_session_recovery_test.cpp#L822) covers schema/reopen behavior; [private_trade_replay_regression:227–269](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_session_recovery_test.cpp#L227) and [requested_trade_epoch_regression:414–435](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_session_recovery_test.cpp#L414) cover legacy private anchors/LifeNum without public order-log orders. |
| E7 | `command_latency_test` | [run_case:153–186](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/command_latency_test.cpp#L153) checks fake place/cancel completion; [200–210](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/command_latency_test.cpp#L200) checks stage/tracing consistency. It is not a paired baseline-versus-merged p99 experiment. |
| E8 | `instrument_lookup_benchmark` | [run:147–170](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/instrument_lookup_benchmark.cpp#L147) checks staged versus committed status, suspension refusal, restored admission and deleted membership. |
| E9 | `plaza2_generated_metadata_test` | [main:41–48](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_generated_metadata_test.cpp#L41) asserts default orders_log/replID descriptors; [75–90](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_generated_metadata_test.cpp#L75) checks consumed code/name/type against captured server schema. |
| E10 | `plaza2_schema_contract_test` | [main:94–124](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_schema_contract_test.cpp#L94) explicitly includes instr2matching_map among unused fields and requires unused missing/retyped fields to remain accepted; [150–154](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_schema_contract_test.cpp#L150) requires missing/retyped consumed fields to fail IncompatibleScheme. It does not test the new general native row-size/offset guard or descriptor allocation cost. |
| E11 | `plaza2_private_state_visibility_test` | [main:95–103](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_private_state_visibility_test.cpp#L95) asserts staged rows remain hidden until TN_COMMIT and expected committed counts. |
| E12 | `plaza2_private_state_projection_test` | [main:147–149](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_private_state_projection_test.cpp#L147) requires excluded options/multileg/matching/PART data not to enter product state, including matching_map().empty(). |
| E13 | `plaza2_private_state_invalidation_test` | [main:190–198](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_private_state_invalidation_test.cpp#L190) checks MAX clear stays staged until commit; [211–217](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_private_state_invalidation_test.cpp#L211) checks cleared LifeNum domain views. |
| E14 | `plaza2_private_state_transaction_perf_test` | [bridge_owns_callback_values:85–94](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_private_state_transaction_perf_test.cpp#L85) checks owned data and commit visibility; [private_futures_scope_regression:40–57](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/private_futures_scope_regression.hpp#L40) checks ignored matching rows/deletions/clears; [private_refdata_update_regression:113–157](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/private_refdata_update_regression.hpp#L113) checks current reference state/stable publication pointers and bounded direct-bridge paths. It does not drive runtime scheme OPEN or assert new metadata tokens. |
| E15 | `plaza2_runtime_listener_test` | [run:87–107](https://github.com/Ruhkukah/Connectors/blob/986f1dce89a9471bd0a6a24df7b9500e0bf949af/tests/plaza2_cgate/plaza2_runtime_listener_test.cpp#L87) checks ordinary REFDATA/AGGR callbacks, decoded UTF-8/raw bytes and inclusion of deliberately slow callback work in timing; not all-stream native binding/ABI equivalence. |

Build wiring is also shared: `connectors/connector_host/CMakeLists.txt` adds the FullLog library to PUBLIC dependencies even for a trading build. `apps/CMakeLists.txt` and `protocols/plaza2_cgate/CMakeLists.txt` add targets; `tests/CMakeLists.txt` appends registrations. Successful unchanged baseline builds/tests establish that the old targets build and execute, not that the resulting artifact/link closure is identical. The four modified shared test/fixture files are not trading-production code; the independent baseline run replaced them with the exact PR68 originals. No production neutrality claim is inferred from these build/test-only files.

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

Fresh local validation completed:

| Check | Result |
|---|---|
| Rebased Release, including real owner-loop benchmark | 46/46 passed |
| Rebased AppleClang ASan/UBSan | 45/45 passed |
| Exact PR68 C++ tests against rebased production | 39/39 passed; sources and original timeouts unchanged |
| Current PR68 rehearsal safeguards | 235/235 passed |
| Pinned clang-format 18.1.8 | 38 changed C++ files passed |

The compiled Release/sanitizer suites and benchmark identify production commit `3871650d`, rebased onto the then-current PR68 `3c525047`. PR68 advanced during validation to `986f1dce` with only a runbook and nine rehearsal Python files. Rebased production, CMake, CI and C++ tests are byte-identical after this second rebase; the new safeguards were run separately. The baseline shadow tree uses the same production and exact PR68 tests/scripts, verified against Git blobs; it is explicitly a mixed validation source, never a deployment candidate. No PR68 checkout, certified binary, TEST deployment or service was changed by that offline validation. The later approved vendor-router provisioning below is separate and does not install a PR69 connector binary into certification.

Measured on local macOS/AppleClang with fake CGate: **4,521,560 input rows/s**, **452,156 admitted rows/s**; callback-to-book p50/p99/max **0.042/0.125/43.041 microseconds** across one million input rows, including filtered rows. Commit-to-DTC-queue p99 **3.375 microseconds**; full 64-level snapshot **4.167 microseconds**; zero row heap allocations. Book storage **31,462,220 bytes**, peak macOS RSS **91,455,488 bytes**. No TEST-host or vendor-native throughput claim follows from this local run.

The benchmark uses the actual ConnectorHost/FullOrderLogDtcLoop and a real loopback DTC drain: 5,000 REFDATA rows, 100,000 retained orders, one million input rows, 90% filtered ISINs, admitted adds/cancels/trades in a 40/40/20 mix with level churn. Report input and admitted rows/s separately, callback-to-book p99, commit-to-DTC-queue p99, memory and row allocations. These are offline fake-CGate measurements.

## Router provisioned and stopped; start not authorized

The owner explicitly approved provisioning a separate TEST vendor router for endpoint `127.0.0.1:4102`, with its own binary/libraries, config, private secret and logs. Provisioning is complete; the secret file is owner-only (`0600`), the root is `0700`, and no router process, service, timer or cron was started. File fingerprints, existing router PID/start identity and existing listeners were unchanged; port4102 has no listener. Private deployment paths and credentials remain outside Git.

A `START_NOT_AUTHORIZED` marker remains in the private run directory. Starting requires a separate owner confirmation of a date without a PR68 session. The stopped installation has not authenticated or been smoke-tested; native loopback binding must be verified before any authorized startup. No unsupported bind option, firewall change or native process execution was used to claim that property. This provisioning does not authorize live order-log observation or deployment of a merged trading binary.

Keep order-log processes off TEST during PR68 interruption/certification sessions and do not run on a PR68 session day. Local schedule documents do not establish a future eligible date; the proposed next short session and MOEX certification day are unarmed/unconfirmed.

After authorization and schedule clearance, observe for 60 minutes in the main session. Require snapshot then ONLINE, retain unknown-order/reopen counters, record LifeNum listener/table identity where the native API provides it, every ClearDeleted table/revision and the matching committed info life. Compare each raw ORDBOOK republish only at its exact linked native log life/revision; report any unaligned interior target or administrative fence as incomplete. Capture live msg/s, moment_ns exchange-to-commit lag, CPU and RSS. The diagnostic comparator always leaves P9/overall qualification inconclusive; a native LifeNum with no table identity must be labelled unavailable.

After certification and an approved merge, the merged trading binary must first pass the PR68 live passive cycles, actual fills, restart/recovery and shutdown checks before trading use. A separate later overlap test also needs its own approval and PR68 process/metric provenance. Compare place-to-post p99 against the same workload with order-log running and SIGSTOPped. It is separate from the isolated observation. No order-log certification or formal R7 claim is made.

## Kairos PR111

PR111 remains OPEN and unmerged at `a5bbda87fdf5c1ca339507ac5923ee78eaa3b82d` on the MOEX branch stack, with no attached GitHub checks. Existing evidence records 453 data tests passed (7 ignored), 24 focused DTC tests and a release application check. Those historical tests were not rerun for this Connector-only rebase. Owner approval is required before merge.
