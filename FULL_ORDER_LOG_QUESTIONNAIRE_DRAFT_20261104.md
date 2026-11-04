# Full Order Log questionnaire — internal draft, 4 November 2026

Not submitted to MOEX. This is separate from certified AGGR/PR68. The internal T0 checks are not Full Order Log certification or the coordinated100,000msg/s R7 test. Owner/reviewer must choose whether the application submitted for certification includes order entry, then copy the corresponding exact inventory into the official questionnaire.

## Application and isolation

Executable: `moex_full_order_log_dtc_runner`. Current native test pin: source984c26b2183f1fa3284480c6046f2853ade8c24b, binary SHA-256 c8ddfed1f1a5855c5764915ae1111a582e7e1a544b7dac332b0716ee06e64d5b. This document/operator-script commit does not rebuild or replace it.

Dedicated router/login/app_name and journal/ID namespace, separate from certified AGGR. Credentials and private host details are supplied outside the repository. ORDLOG and AGGR currently map to the same account: no concurrent trading producers; limits are account-level. T0 test instance `moex_full_order_log_20261104`, inclusive ext IDs2000120000–2000169999, continuing durable state. PROD instance/account/endpoints are not configured by this draft.

Transport template: `p2tcp://<dedicated-router-host>:<port>;app_name=<unique-instance>;timeout=2000`. Optional local_pass is supplied privately. No certification connection is authorized by a URL template.

## Full Order Log subscription

```text
p2ordbook://FORTS_ORDLOG_REPL_MATCH<matching_id>;snapshot=FORTS_ORDBOOK_REPL_MATCH<matching_id>;online.scheme=|FILE|<absolute-SCHEMES>/full_order_log/online.ini|ORDLOG;snapshot.scheme=|FILE|<absolute-SCHEMES>/full_order_log/snapshot.ini|OrdBook
```

Online client scheme contains exactly `orders_log,sys_events`; snapshot client scheme exactly `orders,info`. p2ordbook does not accept tables=. CGate internally binds streams with snapshot.bind, default info.trades_rev. The application validates info if delivered but does not require an application info callback: hidden unframed snapshot staging becomes visible at authoritative ONLINE; online rows stay transaction/commit atomic. See the reviewed snapshot contract and client-scheme manifest.

- `protocols/plaza2_cgate/schema/full_order_log/online.ini`, sectionORDLOG, SHA-256 e9469c254ff52d895b304c147dfe95f7c5d1f003efec222273c1fec0702b4d78.
- `protocols/plaza2_cgate/schema/full_order_log/snapshot.ini`, sectionOrdBook, SHA-2562479cfc9ca726a3303a9df88fa937a4c3f7c4e8f6743d4c81cdf6f3e6808ef44.
- `manifest.json` records exact vendor schemetool makesrc --output-format ini --tables commands and tool/source hashes. Published TEST9.12 source: [forts_scheme.ini](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/Scheme/9.12.0/forts_scheme.ini), SHA-256 f600dbd66c631246d7b386371ce09447ab82c95d2967dfc3df1ceb25a7322c87. Published schemes are distinguished from negotiated native OPEN receipts.

## Other restricted listeners

Every p2repl listener uses an explicit tables= selection; no client scheme= is combined with it. Lists below are from required_replication_tables plus the FOL-only REFDATA extension. AGGR and public DEALS listeners are disabled in FOL mode.

| Stream URL | Tables | Use |
|---|---|---|
| p2repl://FORTS_REFDATA_REPL | session,fut_instruments,fut_sess_contents,fut_vcb,sys_messages,instr2matching_map | FOL reader/writer metadata and matching resolution |
| p2repl://FORTS_SESSIONSTATE_REPL | session_state | Session authority/readiness |
| p2repl://FORTS_INSTRUMENTSTATE_REPL | instrument_state,sys_events | Current instrument state/readiness |
| p2repl://FORTS_TRADE_REPL | orders_log,user_deal,heartbeat,sys_events | Writer/cancel-only private reconciliation |
| p2repl://FORTS_USERORDERBOOK_REPL | orders,info | Writer/cancel-only startup/periodic own-order snapshot |
| p2repl://FORTS_POS_REPL | position,info | Writer/cancel-only positions and TRADE replay anchor |
| p2repl://FORTS_PART_REPL | sys_events | Writer/cancel-only private lifecycle events; no balance rows projected |

Append `;tables=<comma-separated-list>` to each stream URL above; open mode is snapshot+online, with the existing POS-anchored TRADE replay settings where applicable. Cancel-only opens no FOL/AGGR/DEALS/DTC market-data listener and refuses Add/Move immutably.

`instr2matching_map` is consumed, not an unrestricted extra: committed REFDATA resolves the selected instrument's base-contract mapping through fut_vcb and instr2matching_map. Missing, ambiguous or contradictory mapping waits/refuses rather than guessing or falling back to an unsuffixed stream. Current T0 CNY-12.26/ISIN4350847 resolved matching_id1; record fresh resolution for each future native run/instrument.

Writer additionally opens:

```text
p2mq://FORTS_SRV;category=FORTS_MSG;name=<registered-instance>;timeout=60000;scheme=|FILE|<absolute-SCHEMES>/forts_messages.ini|message
p2mqreply://;ref=<registered-instance>
```

The questionnaire must state the order message families actually enabled/tested: AddOrder, MoveOrder, DelOrder and, only under explicit account-exclusive authority, instrument-scoped DelUserOrders. Read-only qualify/probe have no publisher/order APIs. Do not infer permission for trading from account authentication or a data listener alone.

## Diagnostic-only comparison and display

The separate read-only comparison probe opens the restricted composite URL above and `p2repl://FORTS_ORDBOOK_REPL_MATCH<matching_id>;tables=orders,info`. It has a unique app_name, no publisher and zero order posts. It requires exact exposed life/revision alignment; otherwise comparisons are UNALIGNED. This diagnostic connection must be declared if included in a certification session; it is not silently added to the production application's questionnaire inventory.

DTC display binds localhost only, depth20 by default, with FullOrderLog authority envelope and atomic positional-level batches. Kairos end-to-end validation remains conditional on the reviewed PR111 build and recorded live evidence; do not claim it from connector-only tests.

## To complete before submission

- Official applicant/application/version/contact fields and certification scope (data-only versus data plus order entry).
- Approved production infrastructure/account entitlements and final exact mode-specific subscription inventory.
- Reviewer acceptance of the native evidence, resets/recovery/clearing and comparison limitations.
- MOEX-coordinated R7 date, method and evidence; internal rates do not substitute for it.
- Remaining unobserved restart-with-resting-order behavior: pinned SIGTERM intentionally drains cancellations; a clean shutdown/restart is not proof of reconstruction of a surviving working order.
