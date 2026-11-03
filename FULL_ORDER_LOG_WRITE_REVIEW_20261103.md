# FOL executable TEST order mode, 3 November 2026

The owner selected the ORDLOG account for both market data and orders and confirmed that MOEX enabled its trading rights. This implementation is on PR70 only. **Do not merge it into PR68 or use it to replace the certified AGGR artifact.** No T0 process, router, publisher or order is started by this change. Today's requested before-16:00 window elapsed while this code was being validated.

## Executable and account boundary

`moex_full_order_log_dtc_runner plaza2 qualify` retains the read-only display mode. Its DTC ports remain on loopback and reject order entry. New `plaza2 run` uses the same executable and one authenticated ORDLOG CGate connection for the composite FOL listener, private reconciliation, publisher and replies. It serves the owner-only Unix command socket and stdin; it does not open a DTC server. `plaza2 cmd` refuses an endpoint whose status does not identify `market_data_source=full_order_log`.

The existing `moexctl` operator loop was extracted into `operator_program` with three optional hooks. The AGGR executable invokes it with empty hooks. FOL configures its market-data source and per-minute telemetry; both executables retain the existing command framing, journal, durable identifiers, risk accounting, native replies, signals, cancellation drain and restart reconstruction. FOL never spawns or forwards commands to an AGGR executable.

Shared source changes remain reviewable separately from the FOL app. They are compiled into an AGGR executable rebuilt from this candidate; they are not a claim of unchanged binary or timing behavior. The frozen certified executable is not rebuilt or replaced.

| Shared file | No-FOL assessment | Existing regression evidence |
|---|---|---|
| `apps/moexctl.cpp`, `apps/moexctl_main.cpp`, `apps/operator_program.hpp`, `apps/CMakeLists.txt` | Original operator body retained with empty optional hooks for AGGR; extra hook checks and translation-unit layout change compiled instructions. Expected old functional result, not binary neutrality. | `moexctl_command_channel_test`, `cli_shutdown_observation_test`, `shutdown_signals_test`, `full_order_log_isolation_test`. |
| `connectors/plaza2_trade/include/moex/plaza2_trade/cgate_session.hpp` | New optional provider/configuration and accessor; C++ configuration layout changes. Defaults select the old path. | `plaza2_session_recovery_test`, `trading_host_test`; no exhaustive ABI proof. |
| `connectors/plaza2_trade/src/cgate_session.cpp` | New write permission and admission logic is conditional on FOL settings; empty FOL settings return the old readiness result. Extra branches remain. | `plaza2_session_recovery_test`, `plaza2_schema_contract_test`, `trading_host_test`; new FOL test covers the opt-in path. |
| `connectors/connector_host/include/moex/connector_host/trading_host.hpp` | Getter declaration only; no host data member added. | Existing host/command-channel callers; no cross-version ABI claim. |
| `connectors/connector_host/src/trading_host.cpp` | FOL source status/admission/logging branches are conditional. Explicit poll-metric collection is now preserved; legacy default remains false. Ordinary AGGR functional behavior is regression-tested, not assumed cost-free. | `trading_host_test`, `command_latency_test`, `moexctl_command_channel_test`, `cli_shutdown_observation_test`. |
| `connectors/connector_host/include/moex/connector_host/order_manager.hpp`, `connectors/connector_host/src/order_manager.cpp` | Follow-up fixes accepted mass-cancel fallback for all rebuilt trading profiles: exact scoped witnesses permit DelOrder only; entry stays blocked until terminal. Private layout and behavior change even without FOL. PR68 is not changed. | `order_manager_test` plus `mass_cancel_scoped_trade_regression.hpp`, including negative controls and new/relisted identities. |

The3November follow-up corrects the old mass-cancel tests that expected an empty TRADE commit to release fallback/entry. The stricter regression requires scoped identity evidence and terminal reconciliation. Other legacy contracts remain under the full suite; native performance and merged-binary trading qualification remain separate. The OrderManager fix changes behavior in an ordinary trading binary rebuilt from PR70 too; it is deliberately absent from frozen PR68.

The native FOL capability check permits write mode only with TEST, explicit orders and a configured FOL authority provider. The app additionally requires exactly one of `--live` and `--offline-fake`, a measured `--clock-offset-us`, all existing broker/client/login and registered ID/risk inputs, a distinct app name and a router different from the trading router. Defaults are port4102 and app name `moex_full_order_log`. Initial TEST caps are quantity2, position2 per instrument, five open orders and five commands/s; at most64 selected instruments. Limits are configured explicitly within those caps. Offline mode requires the fake-runtime marker and cannot silently use the vendor runtime.

No production flag or DTC order-entry relaxation was added. Missing inputs are refused before owner files or native handles are opened. The binding source is the restricted20October ORDLOG native USER/REFDATA/PART capture, re-read on3November; the private deployment record retains the evidence hashes and split broker/client binding. It proves that AGGR and ORDLOG share the same T0 account, not native order acceptance. ID allocation remains separate from AGGR state. See the4November runbook for registration, account-wide limits and the prohibition on concurrent AGGR/FOL trading.

## Admission and final native post

Add/Move require the existing current private streams, session/instrument state, POS-anchored TRADE reconstruction, publisher/reply and risk gates. In addition, the composite FOL listener must be ACTIVE and its committed book must be valid, two-sided and uncrossed, with positive quantities, epoch and revision. The committed order session must equal the current committed private session. Liveness is the last successfully handled FOL stream row or transaction commit, at most30s old on the owner steady clock; filtered other-instrument rows and sys_events count, empty `cg_conn_process` calls do not. A valid quiet instrument is not rejected merely because its own last order/commit timestamp is old. `moment_ns` must be available and cannot be more than1second ahead of host UTC; this is a bounded tolerance, not a clock-correction claim. The same proof is checked immediately before native Add/Move posting.

Committed order-session tracking derives from bounded per-order sessions and per-instrument live counts and becomes visible at transaction commit. Unknown, empty, invalid and mixed-session books have no session authority. When old-session orders are removed, surviving new-session orders regain authority without resetting the book. Existing resting orders are not retagged merely because a later row carries a new session; a contradictory update to one retained identity makes that identity unknown until removal.

Cancellation, mass cancellation, reconciliation and shutdown drain remain available if FOL data is invalid. Their existing ownership/private-state checks remain in force. The generic private readiness callback is not made dependent on FOL freshness because cancellation retries also use it.

An incompatible schema at a later FOL OPEN remains a fatal session error under the existing fail-fast policy. The operator loop reports outstanding orders and exits. The new explicit `plaza2 run --cancel-only` recovery uses the same FOL executable, ORDLOG login, journal, identity and private reconciliation without FOL/AGGR/DEALS/DTC. Add/Move are immutably refused even after `kill off`; cancellations remain available. Follow `FULL_ORDER_LOG_CLEANUP_20261104.md`; private-stream failure or an actual position still needs owner/MOEX disposition and is not claimed clean.

FOL lifecycle logging retains source table and revision, LifeNum, ClearDeleted and0xA006 (`REPL:CHANNEL_DISCONNECT_ON_TIMEOUT`), with the existing listener-reopen policy. Routine FOL rows and transaction boundaries do not flood the interaction journal. Minute reports retain native callbacks/s, maximum `cg_conn_process` owner-loop duration, rows/s and exchange lag. Status identifies FOL as its source and reports the committed source session/epoch/revision/timestamps.

## Listener inventory in write mode

These `p2repl` URLs are produced by `required_replication_listener_settings`; FOL REFDATA additionally includes its consumed matching map:

```text
p2repl://FORTS_TRADE_REPL;tables=orders_log,user_deal,heartbeat,sys_events
p2repl://FORTS_USERORDERBOOK_REPL;tables=orders,info
p2repl://FORTS_POS_REPL;tables=position,info
p2repl://FORTS_PART_REPL;tables=sys_events
p2repl://FORTS_REFDATA_REPL;tables=session,fut_instruments,fut_sess_contents,fut_vcb,sys_messages,instr2matching_map
p2repl://FORTS_SESSIONSTATE_REPL;tables=session_state
p2repl://FORTS_INSTRUMENTSTATE_REPL;tables=instrument_state,sys_events
p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;online.scheme=|FILE|<scheme-dir>/full_order_log/online.ini|ORDLOG;snapshot.scheme=|FILE|<scheme-dir>/full_order_log/snapshot.ini|OrdBook
p2mqreply://;ref=<FOL-instance-name>
```

The publisher URL is `p2mq://FORTS_SRV;category=FORTS_MSG;name=<FOL-instance-name>;timeout=60000;scheme=|FILE|<scheme-dir>/forts_messages.ini|message`. No AGGR or public DEALS listener is opened. Scheme provenance and exact filtered-table hashes are in `FULL_ORDER_LOG_SCHEME_REVIEW_20261103.md` and the committed asset manifest; published vendor files are not a negotiated T0 capture.

## Offline evidence and native limit

`full_order_log_trading_test` exercises a single fake native connection, no AGGR listener, FOL admission and immediate-post fences, clears/life changes, stale/future/mixed-session/crossed/one-sided books, missing provider, read-only/PROD refusal and cancellation availability. `full_order_log_trading_runner_test` launches the actual FOL executable, posts a passive fake Add, Move and Cancel, reconciles terminal private state, and checks invalid/stale FOL refusal and required CLI inputs. Fake-only INI bootstrap controls are implemented in the fake library, never in production code.

These are offline functional proofs. They do not establish native account order rights, T0 schema acceptance, a clean observation window, live latency or certification. A native order lifecycle and the separately planned read-only book comparison remain pending. No live testing through the certified AGGR executable is required for this FOL change.
