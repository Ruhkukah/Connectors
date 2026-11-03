# FOL OPEN failure with working orders: TEST cleanup

This procedure is prepared, not running. It applies to the ORDLOG deployment on TEST port4102, using the reviewed PR70 FOL executable. It never starts or modifies the certified AGGR executable or PR68. Resting test orders are Day orders; no GTC/GTD/FOK is permitted. No attempt to obtain a fill is authorized by this document.

## Process, account and state

Recovery process: the **same hash-verified `moex_full_order_log_dtc_runner`**, with `plaza2 run --live --cancel-only`. Login: **`tgfzcm_01h_ORDLOG`**, loaded privately from the saved permanent T0 credential; router: the separate ORDLOG profile on127.0.0.1:4102. Use the original writer's registered instance, broker/client binding, ID range, journal and identity checkpoint. Do not reset, truncate, copy over or replace those files. Use a separate recovery command socket and stderr/client-log files. The journal is appended in place so command identities, Move aliases and unknown results remain available.

Cancel-only mode opens the existing private/status replication, publisher and replies. It opens **no FOL, AGGR, public DEALS or DTC listener**. The immutable native mode refuses Add/Move regardless of the operator kill switch; it still permits existing scoped cancellation/reconciliation. It therefore does not depend on the failed FOL schema opening successfully. Broker/client binding and unused registered-ID capacity must already have passed the order-block preflight.

## Exact sequence

1. Stop all new command producers. Preserve the failing journal, outstanding-order report, native OPEN error, process identity and router/client logs. Do not treat socket `ok`, an accepted186 reply or process exit as evidence that an order was cancelled.
2. Verify the failed writer PID/start identity. If it is still alive, issue one SIGTERM to that exact process and wait for its normal drain/exit. Do not start recovery concurrently against the locked journal/checkpoint. Do not use SIGKILL, unlink its lock or blindly retry an uncertain Add/Move.
3. Keep the dedicated ORDLOG router available only within the authorized block/deadline. Verify the original writer exited and its state lock is released. Keep the original account/instance/range and the same `--state`/`--log`; select a new recovery socket. Load all values from the private deployment record, not command history or credentials in a report.
4. Launch the command below after the order block's explicit start message, as a cleanup continuation. No Add/Move command is sent. Observe all required private snapshots/ONLINE, POS-anchored TRADE replay, current session and unresolved-order reconstruction. If private reconstruction fails, stop improvising and use the escalation below.

```sh
"$BIN" plaza2 run --live --cancel-only --allow-orders \
 --runtime-root "$RUNTIME" --library-path "$CGATE_LIBRARY" \
 --scheme-dir "$SCHEMES" --config-dir "$CONFIG" --expected-release SPECTRA9.12.0 \
 --env-settings-var MOEX_FOL_CGATE_ENV --software-key-env MOEX_FOL_SOFTWARE_KEY \
 --broker-code-env MOEX_FOL_BROKER --client-code-env MOEX_FOL_CLIENT \
 --login-env MOEX_FOL_LOGIN --ext-id-range "$FOL_EXT_ID_RANGE" \
 --router 127.0.0.1:4102 --instance-id "$FOL_INSTANCE" \
 --trading-instance-id moex_t0_qualification_20261021 --trading-router 127.0.0.1:4101 \
 --environment test --isin-id "$ISIN" \
 --max-commands-per-second 5 --max-quantity 2 --max-open-orders 5 \
 --max-position "$ISIN=2" --max-notional "$ISIN=$QUOTE_NOTIONAL_CAP" \
 --clock-offset-us "$CLOCK_OFFSET_US" \
 --state "$ORIGINAL_STATE" --log "$ORIGINAL_JOURNAL" --command-socket "$RECOVERY_SOCKET"
```

5. In the operator terminal use this same FOL binary. Confirm status identifies `execution_mode=full_order_log_cancel_only`; issue `kill on`, then cancel each reconstructed nonterminal client-order ID. Move replacements are cancelled through the manager's current native ID/aliases, not an invented ID. `cancel-all ISIN` without `--sole-instance` cancels only owned reconstructed IDs; it is not instrument-wide DelUserOrders.

```sh
"$BIN" plaza2 cmd --command-socket "$RECOVERY_SOCKET" "status"
"$BIN" plaza2 cmd --command-socket "$RECOVERY_SOCKET" "kill on"
"$BIN" plaza2 cmd --command-socket "$RECOVERY_SOCKET" "cancel $CLIENT_ORDER_ID"
"$BIN" plaza2 cmd --command-socket "$RECOVERY_SOCKET" "cancel-all $ISIN"
"$BIN" plaza2 cmd --command-socket "$RECOVERY_SOCKET" "status"
```

6. Require definitive current private evidence: all own scoped orders terminal, no unresolved/pending cancellation, relevant USERORDERBOOK/TRADE reconciliation complete, and the expected position recorded. A heartbeat, unrelated TRADE commit or fresh but still-working row does not reopen Add permission after mass cancellation. If an order remains working, the existing bounded per-ID cancellation policy applies; no unbounded resends or new Add to compensate for uncertainty.
7. Once cancellation is complete, send SIGTERM to the verified recovery process and require its drain/exit. Then SIGTERM the verified ORDLOG router and require zero clients/listeners/processes. Preserve journals/checkpoints and both native logs. A clean result requires terminal-order and flatness proof, exit status, and independent zero-client inspection; otherwise report FAIL/incomplete.

## If private recovery cannot cancel

Do not use the certified AGGR binary as an emergency test or invent a different account. Escalate to the owner/MOEX support with the T0 ORDLOG login, verified broker/client binding, instrument, session, exact public/private order IDs and last native replies/commits; include sanitized router/client logs. The owner must use the exchange/broker's confirmed cancellation channel for this account if private CGate recovery is unavailable. No such external terminal/channel has been verified in this preparation, so it is not represented as an executable fallback.

The cancel-only process cannot flatten an actual position because Add/Move are blocked. Any unexpected fill is recorded, stops further entry and requires the owner's explicit disposition/flattening authority; it is never called a flat/clean test. The block's TERM deadline still applies: report unresolved orders/positions prominently if cleanup cannot finish before the hard15:45MSK stop. Day expiry is not substituted for observed cancellation.
