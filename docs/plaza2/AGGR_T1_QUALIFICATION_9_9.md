# Current SPECTRA 9.9 Aggregated-mode T1 qualification runbook

This is the current runbook for the next open TEST session. It is additive to the dated historical
qualification preparations; those files and their evidence are not rewritten. The runbook uses the
candidate branch and the exact matrix in `cert/AGGR_CERT_MATRIX_9_9.md`.

The session is closed today. Do not start a connection, order, restart, or router mutation until
fresh T1 availability and current source/binary/runtime/scheme/config hashes have been recorded.

The `C01-C08`, `R01-R08`, and `S01-S07` labels below are internal stable IDs mapped one-to-one to the
numbered Plaza II controls in Appendix 1 of the pinned [MOEX Customer Software Certification Procedure](https://www.moex.com/files/4qg0gqtzcxkep68687ah1bwq5e).
They are repository traceability labels, not MOEX-named identifiers. The exact authority pins and the
VPTS technical-requirements pin are in the manifest.

## Reply semantics

The checked-in SPECTRA 9.9 transactional lock is authoritative:

| Command | MsgID | Ordinary business reply | System replies |
|---|---:|---:|---|
| AddOrder | 474 | 179 | 99, 100 |
| DelOrder | 461 | 177 | 99, 100 |
| DelUserOrders | 466 | 186 | 99, 100 |

DelUserOrders is known but not declared for this AGGR candidate. Reply 99 or 100 is never ordinary
success merely because it is correlated with a command. Decode its actual CGate meaning, retain the raw
reply and causal state, and reconcile exchange/private state before allowing any next action.

The only ordinary lifecycle claim is:

```text
AddOrder 474
  -> {correlated business reply 179 + exact matching private Working evidence}
  -> DelOrder 461 (only after both Add facts agree)
  -> {correlated business reply 177 + exact matching private Cancelled evidence + zero active own orders}
  -> reconcile final position
```

Each brace group is a conjunction, not a required arrival order; either may arrive first across the two channels.
Working may precede reply 179 or reply 179
may precede Working; Cancelled may precede reply 177 or reply 177 may precede Cancelled. The explicit DelOrder
is sent only after the Add conjunction is complete. Ordinary cancel success is reported only after the DelOrder
conjunction is complete. System replies 99/100 are exceptional outcomes requiring decoding and reconciliation,
never ordinary business success.

Do not flood or deliberately create an ambiguous live order to manufacture 99/100. Deterministic
reply-bridge tests cover those system replies; any real occurrence is still recorded and reconciled.

## Principal campaign

1. Refresh MOEX notices, T1 availability, the [official T1 listing](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test),
   and the [published schedule](https://www.moex.com/s438). Discover the current symbol, ISIN and session.
   Record source SHA, Linux binary SHA, runtime/library SHA, scheme SHA, router SHA and protected
   configuration fingerprints. A new package listing requires a fresh lock before use.

2. Run the independent C03 probe for at least 300 seconds. It uses no publisher, no listeners and no
   order authorization. Record every poll/state transition, router connection continuity, CPU and
   process exit. The probe must not share mutable evidence with the production host.

3. Start the persistent ConnectorHost from the same reviewed candidate. Establish fresh POS/PART,
   TRADE, USERORDERBOOK, REFDATA, SESSIONSTATE, INSTRUMENTSTATE, AGGR20, publisher and reply state.
   Require current effective readiness, exact account/PART identity, zero orders, zero position and
   valid current price terms before any order gate can open.

4. Run one quantity-1 `FIRST_ORDER_DEEP_PASSIVE_V1` lifecycle. Use the most passive safe price,
   exact current BBO and price-bound checks. Submit one AddOrder and wait until both business reply 179
   and exact matching private Working evidence are present; either channel may arrive first. Only then
   issue one immediate DelOrder. Wait until both business reply 177 and exact private Cancelled evidence
   with zero active own orders are present; either channel may arrive first. Reconcile the final position.
   Any 99/100 or other ambiguity stops new commands and becomes an explicit evidence outcome.

5. After the ordinary lifecycle reaches a safe terminal state and the account is flat or otherwise
   known, perform a controlled application restart with zero active orders. Use the exact binary,
   configuration fingerprints and durable journal. Prove fresh POS-to-TRADE anchoring, every stream
   snapshot/ONLINE/readiness gate, zero unintended posts and final reconciliation.

6. Only after the zero-order restart passes, run the one-Working-order process-restart scenario:
   preserve the Working epoch, stop the process, restart the exact candidate, rediscover and reconcile
   the existing order, prove zero duplicate Add, issue one explicit recovered Cancel, require
   business reply 177 and private Cancelled evidence, then prove zero orders and known position.
   Never retransmit an uncertain Add and never auto-flatten.

7. Qualify local router/network recovery separately. First perform the zero-order local-router
   loss/restart case. Only after it passes may the Working-order case be attempted. For C05/C06/C07,
   first attempt the exact client-controlled T1 equivalent:

   - keep the dedicated local P2MQRouter running and block only its outbound traffic to the discovered
     T1 Plaza access endpoint IP/ports; connector-to-router traffic remains intact;
   - C05: connect and prove router availability with upstream Plaza unavailable, wait/no-send and no
     current-stream use;
   - C06: remove the targeted block and prove the same connector detects upstream availability, performs
     fresh bootstrap/snapshot/ONLINE and reaches readiness without application restart;
   - C07: with zero active orders and known position, reapply the block, prove immediate effective
     readiness/command loss and no stale-stream use, then remove it and prove bounded fresh rebootstrap.

   Discover endpoints from protected configuration without printing credentials. Block only MOEX
   destination IP/port traffic for the dedicated router, never SSH or unrelated VPS traffic. Schedule
   automatic rollback before applying a rule, record original network state and verify rollback after
   each case. Do not run the Working-order transport-loss case until zero-order recovery passes. If an
   exact upstream-only fault cannot be induced with a demonstrably safe reversible mechanism, do not
   improvise: preserve offline evidence and classify only that live portion `MOEX_COORDINATED` with
   the technical reason.

8. Keep the principal observer alive through the published T1 transitions, including the 12:15
   hot-reserve switch. Record exchange/session state and committed generations; do not infer a
   transition from wall-clock time. Exercise only declared AddOrder and DelOrder during safe windows,
   preserving complete logs and order/position reconciliation after every action.

## Evidence and closeout

Each scenario directory is immutable and contains `environment.json`, `scenario.json`, `result.json`,
`events.log`, `metrics.json`, `state_before.json`, `state_after.json` and `hashes.json`.
Include exact reply IDs, raw reply diagnostics, correlating user/reply identity, private Working/Cancelled
rows, order census, position snapshot and source/runtime/config provenance. Record the independent arrival
order of each reply and replication fact; the lifecycle result is based on conjunctions, not timestamps.

At full-day closeout require no unexpected Working orders, a fully reconciled known position, no
unresolved order epoch, no evidence-buffer loss, complete indexed logs and graceful shutdown.
Then run clean Linux Release plus ASan/UBSan/LSan and macOS suites on the exact candidate head.

The report may become `AGGR_READY_FOR_MOEX_CERTIFICATION` only when all software-controlled rows are
`PASS_OFFLINE`, all safely executable current-candidate T1 rows are `PASS_T1`, and only genuine
exchange-controlled rows remain `MOEX_COORDINATED`. Full ORDLOG remains
`DEFERRED_FULL_ORDLOG_PHASE` for a separate tranche.
