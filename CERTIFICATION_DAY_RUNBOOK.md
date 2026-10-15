# MOEX certification day — UNARMED; date to be agreed

**Scope:** one formal, MOEX-agreed Monday–Thursday TEST day. This document
authorizes nothing. The separate Friday short test has 09:00 preflight,
10:00 earliest entry, 14:30 entry cutoff and 15:00 cleanup; it is not this day.
Submit the questionnaire, candidate/source hashes, declared URLs/threading and
Day/IOC/AddOrder/MoveOrder/DelOrder/DelUserOrders scope to the specialist first.
Quantity-10 partial-fill tooling remains inactive unless separately requested.

**06:40 MSK preflight:** verify exact reviewed binary/green CI, TEST destination,
clock offset, storage, software key, exclusive account ownership, current flatness,
no working/queued/unresolved orders, reservation coverage and sufficient unused
durable IDs. Preserve the allocation/checkpoint. Resolve both ISINs from fresh
committed REFDATA. Confirm native schedule, readiness and two-sided books;
calendar time alone grants no entry. Without profiling, do not use
`book.stream_last_callback_utc_ns` for freshness: it is zero in this candidate.
Use native product readiness and current prices. Load credentials privately; record sanitized
effective limits. Variables below are protected deployment values, never secrets
copied into reports.

**Normal CLI; one continuous process.** No profiling or latency-abort flags:

```sh
"$BIN" plaza2 run --runtime-root "$RUNTIME" --scheme-dir "$SCHEMES" \
 --config-dir "$CONFIG" --env-settings-var MOEX_CGATE_ENV \
 --broker-code-env MOEX_TEST_BROKER --client-code-env MOEX_TEST_CLIENT \
 --login-env MOEX_LOGIN --ext-id-range "$MOEX_EXT_ID_RANGE" \
 --router "$ROUTER" --instance-id "$INSTANCE" --environment test \
 --isin-id "$ALRS_ISIN" --isin-id "$CNY_ISIN" --allow-orders --sole-instance \
 --max-commands-per-second 5 --max-quantity 2 --max-open-orders 5 \
 --max-position "$ALRS_ISIN=2" --max-position "$CNY_ISIN=2" \
 --max-notional "$ALRS_ISIN=$ALRS_CAP" --max-notional "$CNY_ISIN=$CNY_CAP" \
 --clock-offset-us "$CLOCK_OFFSET_US" --command-socket "$SOCKET" \
 --state "$STATE" --log "$DAY_LOG"
```

Keep verified PID/start identity separately. In another operator terminal:

```sh
cmd() { "$BIN" plaza2 cmd --command-socket "$SOCKET" "$1"; }
cmd "status"
cmd "place $ID $ISIN buy 1 $PASSIVE day"
cmd "move $ID 1 $NEW_PASSIVE"
cmd "cancel $ID"
cmd "place $IOC_ID $ISIN buy 1 $ASK ioc"
cmd "place $FLAT_ID $ISIN sell $ACTUAL_POSITION $BID ioc"
cmd "cancel-all $ISIN"
```

These are individually confirmed commands, not an unattended batch. Use fresh
prices, unique IDs and unchanged aggregate limits; flatten only actual exposure.
After each request verify native reply, later commit and resulting state;
socket `ok` alone is insufficient. Record Move aliases and exact-once deal/POS
accounting, including zero-fill IOC cancellation.

**Day coverage:** idle five minutes. Exercise the script on both instruments in
main and evening. For DelUserOrders, first confirm three simultaneous working
orders across both instruments within all caps, then cancel each scope. Keep
one eligible passive Day order across **15:00 settlement**; record survival,
relisting or cancellation against committed events and reconstructed state.
Stop entry **15:40**, cancel/flatten by **15:50**, and stay connected read-only
through committed **16:00 clearing**. Then send one SIGTERM to the verified PID.

**Operator decisions:** follow the MOEX specialist's in-scope ad hoc requests.
No rehearsal verdict, latency threshold or missing optional observation aborts
the day. Native ownership/readiness/risk/storage protections remain enforced.
Stop new exposure on uncertain commands, gaps or mismatches; reconcile before
continuing. Never retry an uncertain Add or improvise router faults/restarts.

**Close and submit:** require cancellation, complete flatness, exit 0, zero
clients and no contradictory guardian/shutdown/inventory failure. Preserve any
failure; use bounded ownership-verified emergency cleanup, never SIGKILL.
Archive application journal/checkpoint, default-level **CGate client log** and
**router log** before rotation. Deliver the command/reply/commit-offset table,
session/clearing revisions, log bytes and terminal proof to MOEX.
