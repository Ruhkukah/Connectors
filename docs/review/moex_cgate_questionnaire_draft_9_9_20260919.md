# MOEX CGate questionnaire — review draft (SPECTRA 9.9)

> Draft only; not a completed or submitted questionnaire. User-owned, legal, consent, distribution,
> and deployment answers remain visible as unresolved. No credentials or private account data belong here.

## Snapshot and identity

- As of: `2026-09-19`.
- Structure preserved: **77 answer fields**, including **33 printed stream checkboxes**.
- PR #66 baseline at review start: `ffa6552c70bf6b16568ba4f7943c3660019519d2`.
- CI run [35445864502](https://github.com/Ruhkukah/Connectors/actions/runs/35445864502) at the exact head:
  `connector-validation` and `component-sanitizers` both **SUCCESS**.
- Deployable Linux binary SHA-256: **not claimed**. CI validated source/tests; optional listener-only binary
  upload was skipped. No deployable Linux binary hash is claimed.
- Historical receipt source prefix `49d73326` is from another snapshot and is not evidence for this head.

## Product and demonstration boundary

- Certificate target: Connector / MoexConnector; legal product name and final release identity require user confirmation.
- Current demonstration: Read-only market-data demo. Kairos is external UI, not certificate target. Order-entry/live qualification is unproven.
- Trading: AddOrder/DelOrder exist only in armed TEST path; disabled in read-only runner and not live-qualified.
- FullOrderLog: Separate deferred phase; not part of the current read-only demonstration.

## Effective receive-scheme policy

Explicit ;scheme= selects the named client scheme; a bare stream URL uses server-side scheme. Listener creation forwards URLs unchanged.

| Profile | Service | Scheme alias | Effective policy |
| --- | --- | --- | --- |
| Four-stream read-only | `FORTS_AGGR20_REPL` | `Aggr` | `CLIENT_EXPLICIT` |
| Four-stream read-only | `FORTS_REFDATA_REPL` | `REFDATA` | `CLIENT_EXPLICIT` |
| Four-stream read-only | `FORTS_SESSIONSTATE_REPL` | `server default` | `SERVER_DEFAULT` |
| Four-stream read-only | `FORTS_INSTRUMENTSTATE_REPL` | `server default` | `SERVER_DEFAULT` |
| Four-stream read-only | *initial open arguments* | — | `mode=snapshot+online` |
| Eight-stream ConnectorHost | `FORTS_TRADE_REPL` | `Trade` | `CLIENT_EXPLICIT` |
| Eight-stream ConnectorHost | `FORTS_USERORDERBOOK_REPL` | `OrdBook` | `CLIENT_EXPLICIT` |
| Eight-stream ConnectorHost | `FORTS_POS_REPL` | `POS` | `CLIENT_EXPLICIT` |
| Eight-stream ConnectorHost | `FORTS_PART_REPL` | `PART` | `CLIENT_EXPLICIT` |
| Eight-stream ConnectorHost | `FORTS_REFDATA_REPL` | `REFDATA` | `CLIENT_EXPLICIT` |
| Eight-stream ConnectorHost | `FORTS_SESSIONSTATE_REPL` | `server default` | `SERVER_DEFAULT` |
| Eight-stream ConnectorHost | `FORTS_INSTRUMENTSTATE_REPL` | `server default` | `SERVER_DEFAULT` |
| Eight-stream ConnectorHost | `FORTS_AGGR20_REPL` | `Aggr` | `CLIENT_EXPLICIT` |
| Eight-stream ConnectorHost | *initial open arguments* | — | `ConnectorHost open_settings is empty; do not infer mode. TRADE may reopen from a POS replstate anchor.` |

The effective-profile guard derives listener policy from configured URLs: four-stream profile = 2 explicit / 2
  server-default; eight-stream profile = 6 explicit / 2 server-default. No URL changed.

### OrdBook alias limitation

Pinned OrdBook: orders/multileg_orders have 17/19 fields; USERORDERBOOK has 39/40. Equivalence is unproven;
  OrderBook is absent. Do not guess-rename or claim full-profile qualification. Validate exact OPEN.
  Four-stream observer has no USERORDERBOOK listener.

## Current scheme inventory, requested streams, and consumed tables

Supported-table inventory uses the pinned SPECTRA 9.9 scheme and excludes two tables documented as removed.
It is reference inventory only: it does not assert a table-filtered request or an actual listener OPEN.
CGate URLs request streams without a per-table filter; exact negotiated OPEN tables remain pending.

### Supported current scheme tables

- `FORTS_AGGR20_REPL` tables: orders_aggr, sys_events
- `FORTS_REFDATA_REPL` tables: brokers_base_contracts_params, clearing_members, dealer, discrete_auction,
  discrete_auction_base_contract, fut_bond_isin, fut_bond_nkd, fut_bond_nominal, fut_bond_registry,
  fut_exec_orders, fut_instruments, fut_margin_type, fut_sess_contents, fut_settlement_account, fut_vcb,
  instr2matching_map, investor, multileg_dict, opt_exp_orders, opt_sess_contents, opt_vcb, rates,
  sess_option_series, session, sma_master, sma_pre_trade_check, sys_events, sys_messages, trade_periods,
  user
- `FORTS_SESSIONSTATE_REPL` tables: session_state, sys_events
- `FORTS_INSTRUMENTSTATE_REPL` tables: instrument_state, sys_events
- `FORTS_TRADE_REPL` tables: heartbeat, multileg_orders_log, orders_log, sys_events, user_deal,
  user_multileg_deal
- `FORTS_USERORDERBOOK_REPL` tables: info, info_currentday, multileg_orders, multileg_orders_currentday,
  orders, orders_currentday
- `FORTS_POS_REPL` tables: info, position, position_sa, sys_events
- `FORTS_PART_REPL` tables: part, part_sa, sys_events

### Stream-level request and product consumption

No current-candidate OPEN receipt exists for these streams.

| Service | Scheme policy | Stream request | Product-consumed tables |
| --- | --- | --- | --- |
| `FORTS_AGGR20_REPL` | `CLIENT_EXPLICIT` | Whole stream; no table-level filter. | orders_aggr, sys_events |
| `FORTS_REFDATA_REPL` | `CLIENT_EXPLICIT` | Whole stream; no table-level filter. | session, fut_instruments, fut_vcb, fut_sess_contents, opt_sess_contents, multileg_dict, instr2matching_map, sys_messages |
| `FORTS_SESSIONSTATE_REPL` | `SERVER_DEFAULT` | Whole stream; no table-level filter. | session_state |
| `FORTS_INSTRUMENTSTATE_REPL` | `SERVER_DEFAULT` | Whole stream; no table-level filter. | instrument_state |
| `FORTS_TRADE_REPL` | `CLIENT_EXPLICIT` | Whole stream; no table-level filter. | heartbeat, multileg_orders_log, orders_log, sys_events, user_deal, user_multileg_deal |
| `FORTS_USERORDERBOOK_REPL` | `CLIENT_EXPLICIT` | Whole stream; no table-level filter. | info, info_currentday, multileg_orders, multileg_orders_currentday, orders, orders_currentday |
| `FORTS_POS_REPL` | `CLIENT_EXPLICIT` | Whole stream; no table-level filter. | info, position |
| `FORTS_PART_REPL` | `CLIENT_EXPLICIT` | Whole stream; no table-level filter. | part, sys_events |

### Historical compatibility names

`FORTS_REFDATA_REPL.fut_intercl_info` and `FORTS_REFDATA_REPL.opt_intercl_info` are **historical
  compatibility-only** for this draft. 9.9 docs mark tables removed. Runtime descriptors persist for
  compatibility, not current requests, consumption, or OPEN evidence.

Official source: [https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/p2gate_en.html](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/p2gate_en.html).

## 77-field answer register

Each row resolves its source, automated test, and live evidence through the evidence reference(s). `NOT_RUN` is not a pass.

### 1a — Название ВПТС

- Proposed answer: Proposed name: MoexConnector; the user must confirm the legal/product name.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required
- Source: `cert/AGGR_CERT_MATRIX_9_9.md`
- Automated test/evidence: GitHub Actions run 35445864502: connector-validation SUCCESS; component-sanitizers
  SUCCESS at the exact recorded head
- Live evidence: CI is not T1 live evidence

### 1b — Текущая версия

- Proposed answer: No release version assigned. Review baseline source head and exact CI run are recorded
  above; no deployable Linux binary hash is claimed.
- Status: `user-input`
- Source: `cert/AGGR_CERT_MATRIX_9_9.md`
- Automated test/evidence: GitHub Actions run 35445864502: connector-validation SUCCESS; component-sanitizers
  SUCCESS at the exact recorded head
- Live evidence: CI is not T1 live evidence

### 1c — Разработчик

- Proposed answer: Developer person/company is user-owned legal information; leave unresolved.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1d — Получатель сертификата

- Proposed answer: Certificate holder is user-owned legal information; leave unresolved.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1e — Год создания

- Proposed answer: User to confirm; do not infer from repository history.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1f — Описание функционала

- Proposed answer: CGate receives SPECTRA data. Demo: read-only DTC/AGGR. AddOrder/DelOrder are separate,
  unqualified TEST path; FullOrderLog deferred.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`,
  `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
- Automated test/evidence: plaza2_test_trade_transport_tests; plaza2_order_lifecycle_scenarios_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `cert/AGGR_CERT_MATRIX_9_9.md`
- Automated test/evidence: GitHub Actions run 35445864502: connector-validation SUCCESS; component-sanitizers
  SUCCESS at the exact recorded head
- Live evidence: CI is not T1 live evidence

### 1g — Предусмотрен ли GUI

- Proposed answer: Kairos is an external demonstration client/UI, not the certificate target. This read-only
  demonstration does not prove an order-entry UI.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 1h — Клиентские подключения к ВПТС

- Proposed answer: DTC accepts one active loopback client, separate from the CGate router. Commercial
  distribution/customer count needs user input.
- Status: `implemented`
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`,
  `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1i — Работа с RFS

- Proposed answer: No for the present SPECTRA derivatives scope; no RFS stream is configured.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 1j — Аутентификация/авторизация

- Proposed answer: TEST credentials are deployment inputs. DTC/local logon is not venue authorization.
  Deployment/account policy needs user decision.
- Status: `user-input`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`,
  `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1k — Вхождение заявителя в группу лиц с биржевыми площадками

- Proposed answer: User/legal answer required; leave unresolved.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1l — Использование на организованных торгах

- Proposed answer: User to confirm intended use and distinguish TEST qualification from any production use; no
  production-use claim is made.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required
- Source: `cert/AGGR_CERT_MATRIX_9_9.md`
- Automated test/evidence: GitHub Actions run 35445864502: connector-validation SUCCESS; component-sanitizers
  SUCCESS at the exact recorded head
- Live evidence: CI is not T1 live evidence

### 1m — Клиенты Московской Биржи, планирующие использовать ВПТС

- Proposed answer: User to identify actual organizations, if any; do not infer from software clients or
  repository users.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1n — Цель использования рыночных данных

- Proposed answer: User to select own trading, internal analytics/back-office, or redistribution. No
  redistribution answer is inferred.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1o — Язык взаимодействия с Plaza-2

- Proposed answer: C++20 is the direct CGate/native interaction layer. Rust/C# downstream consumers do not
  change the direct Plaza-2 language.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 1p — Распространение

- Proposed answer: Private/public noncommercial/public commercial is a user/legal decision; leave all choices
  unresolved.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1q — Поддерживаемые рынки MOEX

- Proposed answer: Proposed certification scope: SPECTRA derivatives. Do not check ASTS or RFS based on
  unrelated source artifacts.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 1r — Режимы: утренняя/дневная/вечерняя

- Proposed answer: Session coverage is a user/product decision. Complete session-transition and live evidence
  are not established.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required
- Source: `cert/AGGR_CERT_MATRIX_9_9.md`
- Automated test/evidence: GitHub Actions run 35445864502: connector-validation SUCCESS; component-sanitizers
  SUCCESS at the exact recorded head
- Live evidence: CI is not T1 live evidence

### 1s — Контактные телефоны

- Proposed answer: User to supply privately; do not commit contact details to public Git.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1t — Согласие на использование данных для консультаций

- Proposed answer: User decision; do not preselect Yes or No.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 2a.i — Имя соединения и URL

- Proposed answer: Use sanitized deployment settings. One local p2tcp CGate connection/eight listeners; DTC is
  separate loopback, not Plaza. Omit credentials.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.ii — Максимальное количество соединений

- Proposed answer: ConnectorHost has one CGate connection and eight listeners. DTC has one active client slot;
  listeners are not router connections.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`,
  `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.iii — Тип соединения TCP/LRPC

- Proposed answer: TCP (p2tcp) is configured; LRPC is not configured or claimed.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.iv — Предназначение соединения

- Proposed answer: Read path: replication/reference/status/private market data. Publisher/replies/order
  commands belong only to the separate trading profile.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`,
  `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
- Automated test/evidence: plaza2_test_trade_transport_tests; plaza2_order_lifecycle_scenarios_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.stream.01 — FORTS_TRADE_REPL checkbox

- Proposed answer: Selected in the eight-stream ConnectorHost profile; own order/trade reconciliation, not
  public tape.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`,
  `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.stream.02 — FORTS_COMMON_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.03 — FORTS_VM_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.04 — FORTS_ORDLOG_REPL checkbox

- Proposed answer: Not selected; public FullOrderLog is a separate deferred qualification phase.
- Status: `planned`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.05 — FORTS_AGGRXX_REPL checkbox

- Proposed answer: Selected as the actual FORTS_AGGR20_REPL service (scheme alias Aggr).
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`,
  `protocols/plaza2_cgate/src/plaza2_aggr20_authority_probe.cpp`
- Automated test/evidence: AGGR20 offline/fixture tests; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.stream.06 — FORTS_VOLAT_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.07 — FORTS_DEALS_REPL checkbox

- Proposed answer: Not selected; the current AGGR read-only product path does not use this separate public
  trade stream.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.08 — FORTS_POS_REPL checkbox

- Proposed answer: Selected in the eight-stream profile; not part of the four-stream day observer.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`,
  `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.stream.09 — FORTS_RISKINFOBLACK_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.10 — FORTS_FEE_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.11 — FORTS_PART_REPL checkbox

- Proposed answer: Selected in the eight-stream profile.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`,
  `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.stream.12 — FORTS_RISKINFOBACH_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.13 — FORTS_FEERATE_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.14 — FORTS_REFDATA_REPL checkbox (first printed entry)

- Proposed answer: Selected in both profiles; configured with explicit client scheme alias REFDATA.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`
- Automated test/evidence: connector_host_test effective-profile guard; plaza2_scheme_drift_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; no negotiated listener OPEN receipt
- Source: `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`,
  `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`
- Automated test/evidence: runtime scheme-lock checks; connector_host_test and AGGR fixture tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; exact delivered table inventory pending OPEN

### 2a.stream.15 — FORTS_INFO_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.16 — FORTS_BROKER_FEE_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.17 — FORTS_MISCINFO_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.18 — FORTS_TNPENALTY_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.19 — FORTS_BROKER_FEE_PARAMS_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.20 — FORTS_MM_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.21 — MOEX_RATES_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.22 — FORTS_USERORDERBOOK_REPL checkbox

- Proposed answer: Selected in the eight-stream profile. OrdBook equivalence to USERORDERBOOK layout is
  unproven; do not claim profile qualification.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`
- Automated test/evidence: connector_host_test effective-profile guard; plaza2_scheme_drift_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; no negotiated listener OPEN receipt
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`,
  `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.stream.23 — FORTS_CLR_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.24 — FORTS_FORECASTIM_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.25 — FORTS_ORDBOOK_REPL checkbox

- Proposed answer: Not selected. This is not the configured FORTS_USERORDERBOOK_REPL service; do not conflate
  names.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`
- Automated test/evidence: connector_host_test effective-profile guard; plaza2_scheme_drift_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; no negotiated listener OPEN receipt

### 2a.stream.26 — RTS_INDEX_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.27 — ASTS (MCX) stock streams checkbox

- Proposed answer: Not selected; ASTS is outside the stated SPECTRA derivatives scope.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.28 — ASTS (MCX) FX streams checkbox

- Proposed answer: Not selected; ASTS is outside the stated SPECTRA derivatives scope.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.29 — RFS_INFO_REPL checkbox

- Proposed answer: Not selected; RFS is outside scope.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.30 — RFS_FINESLEVEL_REPL checkbox

- Proposed answer: Not selected; RFS is outside scope.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.31 — RFS_PENALTY_REPL checkbox

- Proposed answer: Not selected; RFS is outside scope.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.32 — RFS_USERMARKETDATA_REPL checkbox

- Proposed answer: Not selected; RFS is outside scope.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.33 — FORTS_REFDATA_REPL checkbox (second printed entry)

- Proposed answer: Preserved as printed; same REFDATA stream as entry 14, not a second listener or separate
  table request.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`,
  `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`
- Automated test/evidence: runtime scheme-lock checks; connector_host_test and AGGR fixture tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; exact delivered table inventory pending OPEN

### 2b.I — Number of CGate-calling threads

- Proposed answer: One owner thread/processes CGate objects for the configured session; downstream DTC/client
  workers do not call CGate.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2b.II — Threads and purposes

- Proposed answer: CGate owner polls and updates state; commands are separate armed profile only. Observer/DTC
  do not publish.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`,
  `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2b.III — conn_process cadence

- Proposed answer: Do not infer cadence from sleeps. Calls/sec, max gap, and idle/active/recovery callback
  budget: NOT_RUN.
- Status: `planned`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2b.IV — CGate objects used from multiple threads

- Proposed answer: No in the single-owner design; retain thread-owner assertions and do not share native CGate
  objects across workers.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2c.I.1 — Replication stream names and initialization URLs

- Proposed answer: Use both inventories. Status streams are ‘Другие потоки’. Sanitize deployment endpoints.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`
- Automated test/evidence: connector_host_test effective-profile guard; plaza2_scheme_drift_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; no negotiated listener OPEN receipt
- Source: `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`,
  `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`
- Automated test/evidence: runtime scheme-lock checks; connector_host_test and AGGR fixture tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; exact delivered table inventory pending OPEN

### 2c.I.2 — Open/subscription mode per stream

- Proposed answer: Observer opens snapshot+online. ConnectorHost mode is unknown until OPEN; TRADE may reopen
  from POS replstate.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`,
  `connectors/connector_host/src/operator_config.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2c.I.3 — Are stream data preserved between process runs

- Proposed answer: Projected state is in-memory and rebuilt by snapshot after restart; archives/journals are
  not resumable stream replicas.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`,
  `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2c.II — Handled CGate messages

- Proposed answer: Callbacks: OPEN/CLOSE, transaction begin/commit, data, ONLINE, LIFENUM, CLEARDELETED,
  REPLSTATE, TIMEOUT; retain native evidence.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2c.III — Message interpretation method

- Proposed answer: Runtime scheme validates layouts; mapped values feed projection. Raw decoding is
  diagnostic; not all rows use packed structs.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`,
  `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`,
  `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2d.I — Commands sent to the trading system

- Proposed answer: Only AddOrder/DelOrder exist in armed TEST path. Read-only observer/DTC has no
  publisher/order surface; other commands are not claimed.
- Status: `implemented`
- Source: `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`,
  `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
- Automated test/evidence: plaza2_test_trade_transport_tests; plaza2_order_lifecycle_scenarios_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`,
  `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2d.II — Supported reply messages

- Proposed answer: AddOrder/DelOrder replies 179/177 differ from system replies 99/100. System replies are not
  ordinary success. T1 replies: NOT_RUN.
- Status: `implemented`
- Source: `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`,
  `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
- Automated test/evidence: plaza2_test_trade_transport_tests; plaza2_order_lifecycle_scenarios_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2d.III — Message allocation and destruction

- Proposed answer: Publisher uses cg_pub_msgnew/post/cg_pub_msgfree, then closes/destroys its handle.
  Read-only mode creates no publisher.
- Status: `implemented`
- Source: `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`,
  `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
- Automated test/evidence: plaza2_test_trade_transport_tests; plaza2_order_lifecycle_scenarios_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`,
  `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 3a.I — Trading-stream heartbeats

- Proposed answer: TRADE heartbeat rows are consumed for stream health/time in the full profile. No claim is
  made for heartbeat support on unconfigured streams.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`,
  `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`,
  `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`
- Automated test/evidence: runtime scheme-lock checks; connector_host_test and AGGR fixture tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; exact delivered table inventory pending OPEN

### 3a.II — sys_events support

- Proposed answer: Consume AGGR20/TRADE/PART events with generation/session/revision handling. Status
  sys_events are not consumed product tables.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`,
  `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`,
  `protocols/plaza2_cgate/src/plaza2_aggr20_authority_probe.cpp`
- Automated test/evidence: AGGR20 offline/fixture tests; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`,
  `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`
- Automated test/evidence: runtime scheme-lock checks; connector_host_test and AGGR fixture tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; exact delivered table inventory pending OPEN

### 3a.III — Index values

- Proposed answer: No index stream is configured or claimed.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 3a.IV — Market information

- Proposed answer: AGGR20 aggregated book: yes. COMMONS: no. Full order log: deferred; do not select options
  just because listed.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`,
  `protocols/plaza2_cgate/src/plaza2_aggr20_authority_probe.cpp`
- Automated test/evidence: AGGR20 offline/fixture tests; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 3b.I — Calendar-spread trading

- Proposed answer: No calendar-spread trading capability is claimed; decoding multi-leg records alone is not
  spread order support.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 3c.I — Mass-operation parameters

- Proposed answer: No mass-order operation is exposed in the current AddOrder/DelOrder scope.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 3d.I — Venue client-management commands/modes

- Proposed answer: No venue-side client-management command is exposed. This is distinct from downstream DTC
  authentication/authorization.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`,
  `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 4 — Cancel on disconnect / CODHeartbeat

- Proposed answer: No qualified venue COD/CODHeartbeat function is claimed. Local socket disconnect cleanup or
  later manual Cancel is not COD.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN
- Source: `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`,
  `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
- Automated test/evidence: plaza2_test_trade_transport_tests; plaza2_order_lifecycle_scenarios_test;
  connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

## Remaining limits before submission or trading-profile qualification

- Capture the exact current-candidate OPEN and negotiated scheme for each configured listener, especially both
  server-scheme status streams.
- Resolve the `OrdBook` versus USERORDERBOOK layout mismatch from an exact OPEN/client-scheme receipt before
  claiming eight-stream qualification. Do not block the four-stream observer.
- Confirm legal identity, certificate holder, release version, business use, distribution, contacts, intended
  sessions, and consent with the user.
- Do not infer `conn_process` cadence from sleeps; collect a measured active/idle/recovery sample.
- No exchange order, support email, automated questionnaire submission, or live T1 claim is part of this
  draft.

Historical references:
[MOEX CGate client manual — data scheme policy](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/Game/docs/cgate_en.pdf)
[MOEX SPECTRA 9.9 gateway documentation](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/p2gate_en.html).

Machine-readable source: `docs/review/moex_cgate_questionnaire_register_9_9_20260919.json`.
