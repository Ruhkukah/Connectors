# MoexConnector 1.0.0 — CGate certification

The first certificate covers own-account SPECTRA trading over CGate 9.9. It includes TRADE, POS, PART, REFDATA, USERORDERBOOK, AGGR20, SESSIONSTATE and INSTRUMENTSTATE. Declare AddOrder, DelOrder, DelUserOrders and MoveOrder. TWIME, FullOrderLog, public DEALS, COD, RFS, .NET/C ABI and DTC order entry are excluded. Public DEALS remains an optional read-only feature, off by default; its outstanding defects are deferred. Deferred source is preserved at `archive/pre-cgate-certification-remediation-20260930`.

Automated tests demonstrate implementation behavior. They do not certify a release or replace the full MOEX test day. The internal live rehearsal and MOEX scenarios below still require the actual router, vendor CGate, provisioned test account and agreed test schedule. No broker command is authorized merely by running the build or tests.

## September 30 review resolution

| Review issues | Result |
|---|---|
| CERT-1, CERT-3, CERT-4; PERF-3, PERF-4; RT-2 | Persistent session owner, indefinite connection recovery, independent listener transactions/reopens, compatible scheme additions, required-field checks and bounded message processing |
| CERT-2, CERT-5; RT-1, RT-3 | Private order readiness separated from AGGR; committed current-session snapshot validity; per-table ClearDeleted; deletion by replID without a price |
| CERT-6, CERT-7; TRD-4, TRD-5, TRD-10 | Configurable TEST/PROD, router and instruments; current-session membership and terms; one-time startup exposure reconstruction; concurrent orders and order renumbering |
| CERT-8; TRD-1, TRD-2, TRD-3, TRD-6, TRD-8, TRD-9 | Operator rate control and queued commands; repeatable risk-reducing cancellation; no ambiguous Add retry; order-ID/account trade attribution; harmless late replies; IOC type 2; definite-not-sent classification |
| CERT-9; TRD-7; RT-5 | Continuous UTC/MSK journal, durable identifier reservations with group commit, default CGate logging validation; CGate `t` interpreted as Moscow time with milliseconds preserved and `moment_ns` preferred |
| PERF-1, PERF-2 | Indexed private rows, committed row deltas and incremental views; map-based AGGR books; native snapshot and online-update regressions |
| DEALS-1 through DEALS-7 | Deferred as requested; public DEALS is optional and off by default |
| RT-4 | Manual TRADE replay keys retained; actual 9.9 replay revision behavior still requires T1 verification |

The official [certification procedure](https://www.moex.com/files/4xgv6e2x1paqr1zkn2fmq093cj), [technical requirements](https://www.moex.com/files/41w8g1tt63pd9tq9drmk4n3g4z) and [CGate manual](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/prod/docs/p2gate_en.pdf) govern the live rehearsal. Archive-only features and historical process documents are available from the archive tag.

The October 1 follow-up adds regressions for valid mass cancellation, bounded recovery failures, participant/session/order ownership, Move fills racing reply 176, eligible command queues, bounded processing, scheme incompatibility latching, revision resets, online private-state updates and identifier checkpoints. `trading_day_test` runs the production owner with an explicitly controlled fake CGate library and verifies `scripts/reconstruct_orders.py` against its journal. This is simulated implementation evidence. Linux/vendor-CGate capacity, actual TRADE replay-key behavior, DTC startup during an already-running T1 session and the complete live day still require deployment verification.

October 1 follow-up validation: all 32 tests passed in native macOS Release and all 32 passed under AppleClang ASan/UBSan. The two loopback socket tests required local bind permission. The production-only Release build passed with 19 compilation units and no fake/test/TWIME/ABI sources; formatting and whitespace checks passed. Darwin LeakSanitizer is unavailable. Ubuntu CI runs separate Release and ASan/UBSan/LSan jobs; its results are build/test evidence rather than vendor-CGate qualification.

## Checks and implementation

| Check | Feature or automated check | Live verification |
|---|---|---|
| C1, C2 | Configurable `p2tcp://HOST:PORT`; one owner thread; `connector_host_test`, `trading_host_test` | Confirm URL/thread answers against CGate log |
| C3 | Zero-timeout processing bounded to 100 calls or 5ms per owner turn; idle wait at most 50ms; continuous-flow regression in `plaza2_session_recovery_test` and command dispatch in `trading_day_test` | Five idle minutes without router loss; cancels/kill responsive under vendor traffic |
| C4–C8 | Nonblocking start; indefinite OPENING; CLOSED/open and ERROR/close loops; same env/connection throughout recovery; `plaza2_session_recovery_test` | Router and upstream stop/start scenarios |
| R1, R2 | Eight configured replication streams on the same owner; `connector_host_test` | Confirm subscription URLs in CGate log |
| R3–R5 | Named fields bound at OPEN; additions ignored; required-field incompatibility latches the listener down for its connection generation; reopen-count regression in `plaza2_session_recovery_test` | MOEX scheme evolution; explicit reconnect/operator correction after incompatibility |
| R6 | Per-listener reopen after one-second pacing; POS-anchored TRADE open; `plaza2_session_recovery_test` | Check replay begins at the POS anchor; verify manual `rev.deal`/`rev.heart_beat` keys on 9.9 |
| R7 | FullOrderLog excluded | Not applicable |
| R8 | Per-table ClearDeleted, including MAX sys_events revision-guard/readiness reset; LifeNum invalidation and committed private deltas; `plaza2_aggr20_md_validation_test`, `plaza2_private_state_transaction_perf_test` | MOEX revision reset scenarios |
| S1, S2, S4 | One publisher and matching p2mqreply; codec/runtime layout checks; `plaza2_trade_command_encoding_test`, `trading_host_test` | Compare configured publisher/send scheme with CGate log |
| S3 | One manager rate gate in the trading host; first eligible cancel before Add/Move; 99 penalty and transition-only throttle logging; `order_manager_test`, `plaza2_session_recovery_test` | MOEX-selected rate and burst |
| S5, S6 | Local 60-second timeout; correlation; malformed 99 consumed immediately; ambiguous Add not resent; failed cancels stop after three attempts with backoff and explicit operator action; fixed first-send absence watermark; `order_manager_test`, `plaza2_session_recovery_test` | Delayed/lost replies and 99/100; authoritative absence/terminal replication |
| S7 | Publisher/reply recovery on their owning session; `plaza2_session_recovery_test` | Publisher outage |
| GEN-1, LOG-1 | UTC/MSK NDJSON with structured commands, key reply fields, private state and exchange announcements; AGGR keeps lifecycle/error events and rare sys_events, excluding book rows and transaction/replay boundaries; buffered writes with 250ms group flush and mandatory durable identity flush before sends; atomic identifier checkpoint with tail-only recovery; active default logging validated for every Live mode; `event_journal_test`, `trading_day_test` | Collect application, CGate client and router logs; verify storage capacity/latency |
| GEN-2a, GEN-2b | Concurrent orders; USERORDERBOOK startup reconstruction followed by live TRADE; cancels after recovery; `order_manager_test`, `trading_host_test` | Restart with two working orders and a partial fill |
| GEN-2c–e | LifeNum/reload recovery; configurable router; manual reserve-server switching | MOEX-coordinated TCS restart/reload/reserve switch |
| GEN-3 | Own trading only; broker-client administration not declared | Not applicable |
| GEN-4 | Exchange stream/table/command names retained | Review terminology |
| DAY-1 | `trading_day_test`: compressed morning/day/clearing/evening, concurrent orders, racing fill/Move, cancel-all under kill, id_ord1 relist, restart with two working orders and native cancel, lost Add reply and three rejected recovery cancels followed by 130s without resubmission; history reconstruction verified | Complete actual morning/day/evening MOEX test day and all declared commands |
| PERF-1 | `plaza2_private_state_transaction_perf_test`: 150,000 decoded TRADE rows through the production bridge; existing-order update and first subsequent insertion, including commit, span access and delta consumption, each below 1ms in Release | Vendor binary decode on deployment hardware, sustained traffic and full-day storage load |
| T-2.5, T-2.6 | Admin/emergency instructions below; stable `--instance-id` in connection and startup log | Confirm deployed identity and emergency access |
| T-2.7 | UTC and MSK timestamps in microseconds; startup records measured clock offset or explicitly unavailable | Verify clock synchronization within one second on the deployment host |
| T-2.8 | `sys_events` and `sys_messages` recorded in the interaction log; `trading_host_test` | Receive exchange/NCC announcements |

## Questionnaire answers

- Product/version: MoexConnector 1.0.0. Developer, certificate holder, contact details and legal declarations remain owner-supplied.
- Purpose: own trading only. Item 1h: no broker/customer connections to the certified trading driver; optional local DTC visualization is a separate read-only interface. Item 1n: market data for own trading. Item 1p: deployment for own use; no broker/client service declared. Legal distribution rights are a separate owner answer.
- Direct gateway language: C++20. One TCP CGate connection; one owner thread uses the connection, listeners and publisher. The driver rejects calls from other threads before querying CGate.
- Connection: `p2tcp://<configured-router>;app_name=<stable-instance-id>;timeout=2000`. Specify the actual deployment address/instance in the submitted form.
- Trading listeners: `p2repl://FORTS_TRADE_REPL`, `FORTS_USERORDERBOOK_REPL`, `FORTS_POS_REPL`, `FORTS_PART_REPL`, `FORTS_REFDATA_REPL`, `FORTS_AGGR20_REPL`, `FORTS_SESSIONSTATE_REPL`, `FORTS_INSTRUMENTSTATE_REPL`. Initial open settings are `mode=snapshot+online`. TRADE opens after POS.info with `lifenum` and the manual replay revision keys. Read-only DTC uses server schemes; the trading profile uses configured client aliases for TRADE/USERORDERBOOK/POS/PART/REFDATA/AGGR and server schemes for status streams.
- Publisher: `p2mq://FORTS_SRV;category=FORTS_MSG;name=<instance>;timeout=60000;scheme=|FILE|<scheme-dir>/forts_messages.ini|message`. Reply listener: `p2mqreply://;ref=<instance>`.
- Polling: process up to 100 zero-timeout calls or 5ms, then service commands, kill/input and other owner work; wait at most 50ms when idle.
- Interpretation: **Анализ схемы**. Required field names/types are checked at OPEN; compatible additions are ignored.
- Message creation: **Сообщения создаются перед отправкой и уничтожаются после отправки**.
- Commands/replies: AddOrder/179, DelOrder/177, DelUserOrders/186, MoveOrder/176; system messages 99 and 100; timeout processing. Type 1 is a day limit order, type 2 is IOC.
- Market validity uses the latest committed event_type 1 for the current session, and is invalidated by a later event_type 5. Order readiness uses private stream and session/instrument states independently of AGGR. No fixed session ID is configured.
- Declare morning/day/evening support as implemented; attach the full-day logs before claiming it was rehearsed. Leave FullOrderLog/public DEALS/COD/RFS unchecked.

## Full-day rehearsal

Build Release on the actual Linux deployment host, then run the exact build with the provisioned TEST account. Preserve default router/client logging. Set the instance name, router, instrument list, rate and risk limits explicitly. Example configuration (environment variables carry local settings and account values):

```sh
build/apps/moexctl plaza2 run \
  --runtime-root /opt/moex/cgate --scheme-dir /opt/moex/cgate/scheme \
  --config-dir /opt/moex/cgate/config --env-settings-var MOEX_CGATE_ENV \
  --broker-code-env MOEX_BROKER --client-code-env MOEX_CLIENT \
  --router 127.0.0.1:4101 --instance-id moex_certification \
  --environment test --isin-id <current-ISIN> --allow-orders \
  --max-commands-per-second 5 --max-quantity 3 --max-open-orders 10 \
  --max-notional <account-approved-cap> --log logs/moex_connector.ndjson \
  --state logs/moex_certification.state
```

Measure clock offset with the host's time service; pass `--clock-offset-us` with that measurement. A null/unavailable startup value is not clock qualification. Use `config/cgate.ini` as the default logging example; the environment ini path must be readable and must preserve the default sink/severity.

1. Leave the connector idle for five minutes.
2. Stop/start the router; interrupt/restore its upstream connection. Verify commands stop during loss, the process waits, and private streams/commands resume after ONLINE.
3. Place at least three simultaneous orders. Partially fill one; cancel another; move one. Exercise DelUserOrders through `cancel-all ISIN` as well as DelOrder.
4. Restart the process with two working orders. Use `status` to obtain their recovered client IDs, cancel one, and place a new order. Verify remaining quantities against TRADE.
5. Submit a burst above the selected rate. Verify all queued commands eventually send and every rolling one-second window respects the cap; exercise a flood penalty and cancel retry.
6. Continue through intermediate clearing, main clearing and the evening session without restarting. Verify the new `sess_id` is followed automatically and repeat every declared command.
7. Arrange TCS restart/reload and reserve-switch scenarios with the MOEX specialist. Confirm TRADE replay starts at the POS revision anchor; do not guess new replay keys without observing 9.9 behavior.
8. Keep the CGate client log, router log and application NDJSON. Run `python3 scripts/reconstruct_orders.py logs/moex_connector.ndjson` to reconstruct each order's command, reply, trade and replication lifecycle. Agree the formal MOEX day/schedule after the internal rehearsal.

## Administration and emergency procedure

Run one host per stable instance ID. The log and identity state are exclusively locked. The state defaults to `<log-directory>/<instance-id>.state`; keep the same `--state FILE` across log rotation or directory changes. A valid checkpoint skips the durable log prefix and recovers only its tail. Missing legacy state requires a one-time log scan; damaged state refuses restart. Preserve both files and reconcile before restoring a backup. Retain vendor logging defaults and keep secrets in local configuration. Historical evidence bundles and deferred source are preserved at the archive tag.

The `run` command owns CGate for the entire day. Enter `place ID ISIN buy|sell QTY PRICE [day|ioc]`, `cancel ID`, `move ID QTY PRICE`, `cancel-all ISIN`, `kill on|off`, `status` or `quit`. Move quantity is the logical order's total quantity including confirmed fills; regime 3 excludes fills racing the move. Executions come from deduplicated own user_deal rows, not quantity-minus-rest. Recovered orders with incomplete historical fills permit cancellation but refuse Move. Quantity, aggregate outstanding quote-notional, price/tick bounds and open-order count include all own recovered working orders. Quote notional is not margin or position exposure. Expired/nonmember instruments refuse new orders. An oversized input line reports an error and discards that line while later commands remain available.

For an incident, enter `kill on` to stop new orders, then `cancel-all ISIN` for each configured instrument. The kill switch permits risk-reducing cancels. Confirm terminal order states from TRADE and positions from POS; a successful cancel reply alone is not final proof. If connectivity is down, the process keeps waiting and retains queued cancels. Use the broker/exchange emergency channel when venue state cannot be established. A process stop or socket disconnect is not exchange cancel-on-disconnect.

SIGINT/SIGTERM, `quit`, startup/poll failures and exceptions all stop the host and close CGate, then attempt to flush the journal. Working orders may remain. Shutdown prints outstanding order IDs and states to stderr with that warning. A storage failure blocks new entries and all sends that require durable reservations; shutdown does not bypass this guard to send automatic cancels. Establish venue state through the broker/exchange emergency channel, repair storage and reconcile before restart. After restart, recover IDs from USERORDERBOOK and TRADE before acting. Unresolved Adds use ext_id only with exact participant, session, instrument and direction; established IDs change only through Move reply linkage or documented id_ord1 relisting. Never replace an Add merely because its reply timed out. After three failed cancel attempts, status exposes `operator_action_required` and the order remains Unknown; establish venue state and use the emergency channel before deliberately retrying. Unknown/late replies are logged.
