# Full Order Log restricted subscription review, 3 November 2026

PR70 is a draft marked **DO NOT MERGE INTO PR68 / THE CERTIFIED AGGR LINE**. Its current base exists to review the FOL delta; it must be retargeted to an integration line before landing. PR68, both preserved AGGR executables and their scheme directories are not modified by this change.

## Listener contract

CGate documents `tables=` for `p2repl`, but not for `p2ordbook`. The previous bare composite requested extra tables. Both client scheme parameters are now required; startup rejects the bare URL, extra parameters, unreadable assets and unsafe path delimiters. The default read-only FOL process opens only these listeners:

```text
p2repl://FORTS_REFDATA_REPL;tables=session,fut_instruments,fut_sess_contents,fut_vcb,sys_messages,instr2matching_map
p2repl://FORTS_SESSIONSTATE_REPL;tables=session_state
p2repl://FORTS_INSTRUMENTSTATE_REPL;tables=instrument_state,sys_events
p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;online.scheme=|FILE|<scheme-dir>/full_order_log/online.ini|ORDLOG;snapshot.scheme=|FILE|<scheme-dir>/full_order_log/snapshot.ini|OrdBook
```

The three `p2repl` listeners use `required_replication_listener_settings`; only FOL REFDATA adds its consumed matching map. AGGR, public DEALS, private trading streams, a publisher and a reply listener are not opened in the default read-only mode. The independent test-only native comparison probe keeps its explicit MATCH suffix and restricts raw ORDBOOK to `tables=orders,info`.

The online INI contains exactly `orders_log,sys_events`; the snapshot INI exactly `orders,info`. All native fields in those selected tables are retained. `sys_events` is retained for exchange lifecycle diagnostics; no multileg, heartbeat or current-day table is requested. OPEN requires exactly four table names and the consumed names, wire types, widths and row bounds; it must refuse missing or unexpected tables. Fake-runtime tests interpret the actual INI declarations and filtered indices rather than merely asserting URL strings.

The full `forts_scheme.ini` must remain in the separate FOL `--scheme-dir` for the shared runtime probe, with the filtered files below its `full_order_log/` subdirectory. The two INIs alone are not a complete runtime scheme bundle. A new candidate release must include the assets; certified/vendor shared directories must not be edited.

## Generation and provenance

The files were generated offline by matching official CGate 9.12.0.2144 `schemetool`. Commands and SHA-256 identities are committed in the asset manifest. No native connection was opened. Source is MOEX's published versioned TEST schema; it matches the previously hash-pinned archive. It is **not an actual negotiated T0 server capture**. Vendor headers say `Target poligon:prod` despite distribution under TEST; this distinction is retained. No saved T0 composite capture was found. Actual T0 entitlement and schema acceptance remain pending the separately authorized live review/run.

- Online SHA-256: `e9469c254ff52d895b304c147dfe95f7c5d1f003efec222273c1fec0702b4d78`.
- Snapshot SHA-256: `2479cfc9ca726a3303a9df88fa937a4c3f7c4e8f6743d4c81cdf6f3e6808ef44`.
- Source SHA-256: `f600dbd66c631246d7b386371ce09447ab82c95d2967dfc3df1ceb25a7322c87`.
- Archive SHA-256: `73fb651459fdb1420f82f684e7c2fc3dbaa20dac085f9ff83fdc38fc35e16d61`.

## Test-count explanation

At `6a9245fe`, local CTest registered 47 tests because local configuration explicitly set `MOEX_ENABLE_PERF_TESTS=ON`. The additional test is **`plaza2_full_order_log_perf`**. CI and TEST used the default OFF and registered 46 functional tests. The benchmark executable is still built there; only its sustained performance acceptance is excluded from CTest. This was not a missing functional test or a silently skipped failure. Regression additions within existing test executables do not necessarily change the CTest target count.

## Requested FOL order entry

The owner clarified that future order testing must use a writable FOL executable, not a repeat of the already certified AGGR executable. No AGGR test is prepared or run in this change. The scheme-fix commit remains explicitly read-only; the separate follow-up implements TEST `plaza2 run` by reusing the existing trading host, rather than introducing a raw publisher. See `FULL_ORDER_LOG_WRITE_REVIEW_20261103.md` for that mode's exact boundaries and additional listener inventory.

Reuse `CgateTradingHost`, `OrderManager`, the journal, command socket and shutdown drain. Preserve private TRADE/POS/USERORDERBOOK reconstruction, registered durable IDs, account/instance ownership, replies and risk limits. FOL validity/current session/freshness must gate Add/Move at admission and dispatch, while cancellations and reconciliation remain available when FOL data is invalid.

The owner selected the ORDLOG login for both data and orders and confirmed that MOEX enabled trading rights. The write-mode implementation therefore uses one authenticated native connection. It still requires explicit TEST authorization, broker/client binding, a registered nonoverlapping durable ID range, private-state readiness and bounded risk configuration; authentication alone is not that proof. Default read-only mode and the certified AGGR artifact remain intact. The write integration follows this subscription fix in a separate commit and requires its own offline/native validation. No live FOL process or order is started by this review change.
