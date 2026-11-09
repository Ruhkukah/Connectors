# Full Order Log: AGGR synchronization, 3 November 2026

> Historical merge snapshot at `6a9245fe`: the later client-scheme restriction supersedes the bare composite URI described below. See `FULL_ORDER_LOG_SCHEME_REVIEW_20261103.md` for the current four-table contract.

The owner requested the newly certified AGGR changes on the separate Full Order Log line. The verified current AGGR/PR68 head is `fb57c8f7e3505ad8f41b0191438930192844c24e`. PR70 previously ended at `625e726bd697623c651c48466f91de0c6d72891f`, whose AGGR merge base is `52782266cc9e230dbcbb84bd67433a2febce7d9b`.

This synchronization merges the two intervening AGGR commits:

- `d96a4086`: restrict replication subscriptions and required-table validation to the tables the consumer uses.
- `fb57c8f7`: model excluded-table clears in the fake runtime and retain strict subscription/readiness assertions.

PR68 and its executable are not changed, rebuilt in place, pushed or redeployed. The 2 November public build/native receipts bind the AGGR source above to executable SHA-256 `f6e8c0c6a2cc06b5d6c295923f7ab0fb75890e89923862ab5a9ac0ebedd548ac`. The earlier `5330615f` / `8ab263cc48739d4f76861692e849ca49d755297438297040c27497c785651dbb` artifact is also preserved. The owner's MOEX certification statement applies to AGGR; it does not certify the FOL changes or this merged build.

## Merge and compatibility

The incoming delta touches eleven files. Six overlap the existing FOL changes; only `protocols/plaza2_cgate/src/plaza2_runtime.cpp` has a textual conflict. Its resolution retains AGGR's central required-table validation and the FOL branch's existing raw-consumer handling. The remaining overlapping files merge cleanly: `CERTIFICATION.md`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/cgate_session.cpp`, `tests/connector_host_test.cpp`, and `tests/plaza2_cgate/fake_cgate_runtime.cpp`.

AGGR's default REFDATA subscription remains exactly:

```text
session,fut_instruments,fut_sess_contents,fut_vcb,sys_messages
```

FOL additionally needs `instr2matching_map` to resolve the committed instrument-to-matching binding. A blind merge would filter out that table before the FOL consumer could use it. The compatibility change requests it only for the FOL profile. The vendor composite `p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL` is retained without adding a `tables=` parameter or a client scheme. Runtime and bridge validation continue to reject missing consumed data.

| Shared production file | Result relative to the incoming AGGR head | Default-profile evidence |
|---|---|---|
| `connectors/connector_host/src/operator_config.cpp` | Import AGGR's reviewed subscription helper while retaining the existing FOL/local-password changes. The synchronization adds no further change here. | Incoming `connector_host_test` retains its exact restricted-URL assertions; existing FOL assertions remain separate. |
| `connectors/plaza2_trade/src/cgate_session.cpp` | Import AGGR status subscriptions. Only when a FOL handler is configured, replace the copied REFDATA listener settings with the reviewed six-table selection; custom FOL REFDATA selectors are intentionally replaced. The original config and default five-table helper are unchanged. | `plaza2_full_order_log_session_test` checks the actual filtered six-table FOL schema, exact five-table no-handler schema, matching resolution, and missing matching table/field refusal. `plaza2_session_recovery_test` retains AGGR startup/recovery assertions. |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_replication_tables.hpp` | Byte-identical to AGGR. No FOL flag or extra table is added to the common helper/API. | `plaza2_schema_contract_test` and `connector_host_test` retain the incoming required-table and URL checks. |
| `protocols/plaza2_cgate/src/plaza2_runtime.cpp` | Resolve the conflict using AGGR's required-table loop. Retain the existing FOL raw-consumer exception so the composite book binds its native schema. Non-raw/default validation uses the incoming AGGR list. | `plaza2_schema_contract_test` preserves strict filtered/reordered schema checks; `plaza2_full_order_log_session_test` proves unfiltered composite URI, schema refusal and recovery. |

These are functional/source assessments, not claims of unchanged generated instructions or runtime cost. The incoming AGGR exchange-message, multileg-history, schema-contract and session-recovery test sources are preserved without weakening their assertions. Test-only fake listener-settings capture supplies independent evidence of the actual requested URLs.

The AGGR subscription changes affect shared production files compiled into a rebuilt trading binary. This branch therefore remains a separate candidate. Default-profile regression tests establish the listed functional behavior; they do not prove binary, ABI or latency equivalence. The historical per-file assessment in `PR69_REBASE_REVIEW_20261015.md` remains applicable to the older FOL changes. Before a merged executable is used for trading, the previously agreed passive cycles, fills, restart/recovery and shutdown checks still apply.

## Validation and operation

Validation for the exact final commit is recorded in PR70 and its build/deployment receipts: the complete C++/FOL suite, rehearsal safeguards, Release/sanitizer CI, and a separate offline TEST build. No timeout or readiness condition is relaxed to obtain a pass. Any new FOL executable is copied only into its own immutable release path; no certified trading executable is copied or replaced.

The dedicated T0 ORDLOG account's password was changed privately on 3 November. Native `change_password` reported result code 0 and exit 0; fresh authentication with the saved credential showed the account's own connected-node OTP flag 0. Connection-peer OTP flags are not used to infer the account's password state. The standard vendor T0 profile was required to resolve the password command service; default-only authentication succeeded but could not resolve `FORTS_SRV`. No credential values or software keys are stored in Git.

After password administration, the dedicated router and utility are stopped, port 4102 has no clients/listeners, and FOL remains stopped. Authentication does not prove ORDLOG stream entitlement. No observation window, order, PROD connection, timer or wake-up is authorized by this synchronization. The 1 November plan is retained as historical preparation; a future read-only observation requires a new owner message, current preflight and a new TERM deadline.
