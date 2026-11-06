# Full Order Log questionnaire — internal draft, 4 November 2026

**Crash recovery:** a reconstructed/recovered order is cancel-only: Move is blocked while `execution_baseline_known=false`. Cancel it by its recovered identity; do not resend Add or infer historical fills from remaining quantity. Actual validated fills continue to be accounted exactly once. A new order needs fresh entry readiness and the writer/session gate.

Not submitted to MOEX. This is separate from certified AGGR/PR68. The internal T0 checks are not Full Order Log certification or the coordinated100,000msg/s R7 test. Owner/reviewer must choose whether the application submitted for certification includes order entry, then copy the corresponding exact inventory into the official questionnaire.

## Application and isolation

Executable: `moex_full_order_log_dtc_runner`. Current native test pin: source984c26b2183f1fa3284480c6046f2853ade8c24b, binary SHA-256 c8ddfed1f1a5855c5764915ae1111a582e7e1a544b7dac332b0716ee06e64d5b. This document/operator-script commit does not rebuild or replace it.

Dedicated router/login/app_name and journal/ID namespace, separate from certified AGGR. Credentials and private host details are supplied outside the repository. ORDLOG and AGGR currently map to the same account: no concurrent trading producers; limits are account-level. T0 test instance `moex_full_order_log_20261104`, inclusive ext IDs2000120000–2000169999, continuing durable state. PROD instance/account/endpoints are not configured by this draft.

Transport template: `p2tcp://<dedicated-router-host>:<port>;app_name=<unique-instance>;timeout=2000`. Optional local_pass is supplied privately. No certification connection is authorized by a URL template.

## Full Order Log subscription

```text
p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;online.scheme=|FILE|<absolute-SCHEMES>/full_order_log/online.ini|ORDLOG;snapshot.scheme=|FILE|<absolute-SCHEMES>/full_order_log/snapshot.ini|OrdBook
```

The ORDLOG heartbeat supplies periodic trading-core activity. The entry gate requires a current committed stream and a valid instrument book. Online client scheme contains exactly `orders_log,heartbeat,sys_events`; snapshot client scheme exactly `orders,info`. p2ordbook does not accept tables=. CGate internally binds streams with snapshot.bind, default info.trades_rev. The application validates info if delivered but does not require an application info callback: hidden unframed snapshot staging becomes visible at authoritative ONLINE; online rows stay transaction/commit atomic. See the reviewed snapshot contract and client-scheme manifest.

- `protocols/plaza2_cgate/schema/full_order_log/online.ini`, sectionORDLOG, SHA-256 e9469c254ff52d895b304c147dfe95f7c5d1f003efec222273c1fec0702b4d78.
- `protocols/plaza2_cgate/schema/full_order_log/snapshot.ini`, sectionOrdBook, SHA-2562479cfc9ca726a3303a9df88fa937a4c3f7c4e8f6743d4c81cdf6f3e6808ef44.
- `manifest.json` records exact vendor schemetool makesrc --output-format ini --tables commands and tool/source hashes. Published TEST9.12 source: [forts_scheme.ini](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/Scheme/9.12.0/forts_scheme.ini), SHA-256 f600dbd66c631246d7b386371ce09447ab82c95d2967dfc3df1ceb25a7322c87. Published schemes are distinguished from negotiated native OPEN receipts.

## Recorder/OI source revision (offline until separately approved)

The new FOL source adds opt-in lossless Zstd recording and offline DBN v3 export, plus opt-in `--open-interest` for `p2repl://FORTS_DEALS_REPL;tables=deal` in reader/writer mode (default off). Declare this table only for a run using that flag. It leaves installed/native pins unchanged; the old pin above does **not** include OI. Cancel-only still opens no public market-data streams. Snapshot OI is hidden until ONLINE; subsequent per-trade observations are exposed at transaction commit. `xpos` remains a signed 64-bit market quantity, with `moment_ns`, `sess_id`, `isin_id`, `id_deal` and replication identities validated from OPEN. No OI is inferred from order quantities. The whole selected public deal row is retained if recording is enabled. DEALS requests only `deal`. The composite ORDLOG subscription also consumes `heartbeat` for committed stream liveness. No writer order or risk authority is taken from OI. In read-only qualify mode the same DEALS listener supplies committed public trade prints to Kairos. Startup snapshot trades are withheld from the live subscription. The DTC server waits for current DEALS and depth before granting combined display authority.

## Other restricted listeners

Every p2repl listener uses an explicit tables= selection; no client scheme= is combined with it. Lists below are from required_replication_tables plus the FOL-only REFDATA extension. AGGR is disabled in FOL mode. The recorder/OI revision adds one restricted public DEALS listener only with --open-interest (default off); this source change is not deployed or natively qualified.

| Stream URL | Tables | Use |
|---|---|---|
| p2repl://FORTS_REFDATA_REPL | session,fut_instruments,fut_sess_contents,fut_vcb,sys_messages,instr2matching_map | FOL reader/writer metadata and matching resolution |
| p2repl://FORTS_DEALS_REPL | deal | Optional --open-interest only: public open interest (`xpos`), replication identity, session/instrument and observation time; live DTC trade prints, lossless selected rows and DBN trade/Statistics export |
| p2repl://FORTS_SESSIONSTATE_REPL | session_state | Session authority/readiness |
| p2repl://FORTS_INSTRUMENTSTATE_REPL | instrument_state,sys_events | Current instrument state/readiness |
| p2repl://FORTS_TRADE_REPL | orders_log,user_deal,heartbeat,sys_events | Writer/cancel-only private reconciliation |
| p2repl://FORTS_USERORDERBOOK_REPL | orders,info | Writer/cancel-only startup/periodic own-order snapshot |
| p2repl://FORTS_POS_REPL | position,info | Writer/cancel-only positions and TRADE replay anchor |
| p2repl://FORTS_PART_REPL | sys_events | Writer/cancel-only private lifecycle events; no balance rows projected |

Append `;tables=<comma-separated-list>` to each stream URL above; open mode is snapshot+online, with the existing POS-anchored TRADE replay settings where applicable. Cancel-only opens no FOL/AGGR/DEALS/DTC market-data listener and refuses Add/Move immutably.

`instr2matching_map` is consumed, not an unrestricted extra: committed REFDATA resolves the selected instrument's base-contract mapping through fut_vcb and instr2matching_map. Missing, ambiguous or contradictory mapping waits/refuses rather than guessing the instrument matching ID. In the current pinned writer this is an authority check: its listener URL remains unsuffixed, as confirmed by native cg_lsn_new logs. Matching resolution must not be described as a writer subscription-shard restriction; changing that routing needs separate review. Current T0 CNY-12.26/ISIN4350847 resolved matching_id1; record fresh resolution for each future native run/instrument.

Writer additionally opens:

```text
p2mq://FORTS_SRV;category=FORTS_MSG;name=<registered-instance>;timeout=60000;scheme=|FILE|<absolute-SCHEMES>/forts_messages.ini|message
p2mqreply://;ref=<registered-instance>
```

The questionnaire must state the order message families actually enabled/tested: AddOrder, MoveOrder, DelOrder and, only under explicit account-exclusive authority, instrument-scoped DelUserOrders. Read-only qualify/probe have no publisher/order APIs. Do not infer permission for trading from account authentication or a data listener alone.

## Diagnostic-only comparison and display

The separate read-only comparison probe opens the restricted composite URL above with both stream names explicitly suffixed `_MATCH<matching_id>`, plus `p2repl://FORTS_ORDBOOK_REPL_MATCH<matching_id>;tables=orders,info`. This differs from the current unsuffixed writer URL. It has a unique app_name, no publisher and zero order posts. It compares selected order IDs and remaining quantities against committed composite states in a bounded observation window and reports ambiguous or differing snapshots separately. This diagnostic connection must be declared if included in a certification session; it is not silently added to the production application's questionnaire inventory.

DTC display binds localhost only, depth20 by default, with FullOrderLog authority envelope and atomic positional-level batches. Kairos end-to-end validation remains conditional on the reviewed PR111 build and recorded live evidence; do not claim it from connector-only tests.

## Partitioned candidate inventory

The earlier sections describe the historical native pin. This source revision resolves exactly one matching ID for the selected instruments before opening any partitioned listener. It uses the matching suffix on both public composite services and retains the client file section names `ORDLOG` and `OrdBook`. The two client schemes retain their table selection and digests. The base REFDATA, SESSIONSTATE, INSTRUMENTSTATE and required PART event subscriptions retain their unsuffixed service names.

| Mode | Service | Tables or scheme sections |
|---|---|---|
| Qualify and writer | `p2ordbook://FORTS_ORDLOG_REPL_MATCH<n>;snapshot=FORTS_ORDBOOK_REPL_MATCH<n>` | Restricted client schemes as above |
| Qualify and writer with `--open-interest` | `p2repl://FORTS_DEALS_REPL_MATCH<n>` | `deal`. Writer starts from matching POS `info` LifeNum and deal revision |
| Writer and cancel-only | `p2repl://FORTS_TRADE_REPL_MATCH<n>` | `orders_log,user_deal,heartbeat,sys_events` |
| Writer and cancel-only | `p2repl://FORTS_USERORDERBOOK_REPL_MATCH<n>` | `orders,info` |
| Writer and cancel-only | `p2repl://FORTS_POS_REPL_MATCH<n>` | `position,info` |
| Native qualifier and writer | `p2repl://FORTS_COMMON_REPL_MATCH<n>` | `common`. Compare the native-liquidity bid and ask as an advisory sample |
| Native writer | `p2repl://FORTS_REFDATA_REPL` | Separate `user` listener. Detect COD for the exact configured login |
| Native writer | `p2repl://FORTS_PART_REPL` | Separate `part` balance listener with independent health |
| Native writer | `p2repl://FORTS_VM_REPL_MATCH<n>` | Advisory `fut_vm` for the account and selected futures |
| Native writer | `p2repl://FORTS_PROHIBITION_REPL` | Advisory `prohibition` for the configured account. Enables local enforcement with `--check-account-prohibitions` |
| Native writer | `p2repl://FORTS_RMT_REPL` | Advisory `rmt_im` for the configured account |
| Native writer | `p2repl://FORTS_TNPENALTY_REPL` | Advisory `fee_tn,fee_all` for the configured login |
| Native qualifier and writer | `p2repl://FORTS_REFDATA_REPL` | Separate advisory `trade_periods,discrete_auction` and `spectra_environment` listeners |
| Native qualifier and writer | `p2repl://MOEX_RATES_REPL` and `p2repl://RTS_INDEX_REPL` | Advisory `curr_online` and `rts_index`, respectively |
| Native writer | `p2repl://FORTS_CLR_REPL` | `fut_sess_settl,money_clearing,fut_pos,clr_rate,sys_events` |

The writer fences Add and Move when reference authority is unavailable. It reopens partitioned streams only after a committed mapping changes to a different matching ID. The publisher and reply listener retain their separate reviewed service names. Trading requires validated live command and reply descriptors. The writer refuses Add and Move when the current trading login's COD state is enabled or unknown. Cancel-only remains available with current private authority and validated message layouts.

The optional `--min-free-money-rub` and `--check-account-prohibitions` gates require an active connection, an active listener, and a current ONLINE snapshot. Any applicable nonzero prohibition row for the configured account fences entry conservatively. The venue still enforces its full group, priority and side-specific rules. Both local checks apply to Add and Move. The account balance listener remains independent of the required PART `sys_events` listener.

REFDATA lot volume uses exact scale-eight units. An available extended value must agree with any available legacy integer value. An absent extended field can fall back to a positive legacy integer. Missing or inconsistent lot volume fences entry for that instrument. Exchange order quantities remain whole contracts, and DTC displays the lot volume as a float.

Optional Kairos trading uses an authenticated, loopback DTC endpoint on the writer. It exposes one instrument and uses the same CGate owner and order manager as CLI orders. Reconciliation restores current private state before enabling DTC entry. Manual and automated Adds carry `M` and `R` respectively. Automated orders can be cancelled. Replace is limited to manual orders because DTC CancelReplace carries no automation flag. DTC exposes current account balances from PART, with VM available separately in connector status. By default, after ten seconds following any disconnect of an authenticated trading session without a fully reconciled replacement session, it individually cancels working orders placed through that endpoint when the private state is available. `--dtc-keep-orders-on-disconnect` disables those cancels. A bounded socket worker exchanges snapshots and commands with the CGate owner. A separate persistence worker reserves DTC Add identity before the owner rechecks admission and submits. Cancels have an independent command slot and remain available while an Add awaits persistence. After two seconds without its persistence acknowledgement, the Add fails locally, DTC entry is fenced, and status latches `dtc_identity_failed`; a late acknowledgement cannot post it. Identity records use temporary-file sync, atomic rename and directory sync. On restart, partial temporary records are quarantined, their keys and external IDs remain reserved, and `dtc_identity_quarantined_records` raises an operator alert. Do not delete the ledger or reset the durable ID ceiling to clear an alert. A kernel-stalled disk operation may delay the persistence worker joining after native shutdown; the external process deadline remains required.

Market-frame ring overflow replaces obsolete depth updates with a fresh full snapshot on the same DTC session. A lost public-trade backlog is explicitly reported and its subscription must be renewed; it is not silently presented as complete. A non-reading socket can still be disconnected by the bounded output limit or write timeout, which starts the same cancellation timer. Refused `quit` leaves the process and cancellation frontend alive with the kill switch on. Report the actual DTC options, timing, and cancellation policy if this mode is submitted.

Backfill planning and overlap verification are separate from the live application. The candidate TEST-only collector uses `p2repl://FORTS_ORDLOG_REPL_MATCH<n>;tables=orders_log` with an independently verified LifeNum and earlier committed revision. Declare that subscription only for a separately qualified collector.

## To complete before submission

- Official applicant/application/version/contact fields and certification scope (data-only versus data plus order entry).
- Approved production infrastructure/account entitlements and final exact mode-specific subscription inventory.
- Reviewer acceptance of the native evidence, resets/recovery/clearing and comparison limitations.
- MOEX-coordinated R7 date, method and evidence; internal rates do not substitute for it.
- Remaining unobserved restart-with-resting-order behavior: pinned SIGTERM intentionally drains cancellations; a clean shutdown/restart is not proof of reconstruction of a surviving working order.
