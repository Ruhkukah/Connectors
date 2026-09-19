# MOEX CGate questionnaire — review draft (SPECTRA 9.9)

> Draft only; not a completed or submitted questionnaire. User-owned, legal, consent, distribution,
> and deployment answers remain visible as unresolved. No credentials or private account data belong here.

## Snapshot and identity

- As of: `2026-09-19`.
- Structure preserved: **77 answer fields**, including **33 printed stream checkboxes**.
- PR #66 baseline at review start: `ffa6552c70bf6b16568ba4f7943c3660019519d2`.
- CI run [35445864502](https://github.com/Ruhkukah/Connectors/actions/runs/35445864502) at the same exact head: `connector-validation` and `component-sanitizers` both **SUCCESS**.
- Deployable Linux binary SHA-256: **not claimed**. No deployable Linux binary hash is claimed. The CI run validated source and tests; its optional listener-only capture-binary upload step was skipped.
- Historical receipt source prefix `49d73326` is a different snapshot and is not promoted to this head.

## Product and demonstration boundary

- Certificate target: Connector / MoexConnector; legal product name and final release identity require user confirmation.
- Current demonstration: Read-only market-data demonstration. Kairos is an external client/UI, not the certificate target. The current demonstration does not establish an order-entry UI or live order qualification.
- Trading: AddOrder and DelOrder are implemented in the separate armed TEST trading path; they are disabled in the read-only runner and have no current-candidate live qualification claim.
- FullOrderLog: Separate deferred phase; not part of the current read-only demonstration.

## Effective receive-scheme policy

A p2repl URL with an explicit ;scheme= uses the named client scheme; a bare stream URL selects the server-side scheme. Plaza2Listener::create forwards the configured URL and does not add a scheme.

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
| Eight-stream ConnectorHost | *initial open arguments* | — | `Empty open_settings in the current effective config; do not infer the negotiated mode. TRADE may later reopen from the POS anchor with replstate.` |

The effective-profile guard derives the listener policy from the configured URLs: four-stream profile = 2 explicit / 2 server-default; eight-stream profile = 6 explicit / 2 server-default. No listener URL was changed.

### OrdBook alias limitation

The lock contains OrdBook, but its orders/multileg_orders signatures have 17/19 fields, versus 39/40 on the FORTS_USERORDERBOOK_REPL entries. The lock does not establish these as equivalent layouts; OrderBook is not an observed alias entry in this lock. Do not rename by guess and do not claim trading-profile qualification. Capture and validate the exact current listener OPEN. The four-stream read-only profile has no USERORDERBOOK listener and is unaffected.

## Current scheme inventory, requested streams, and consumed tables

The current supported-table column below is derived from the pinned SPECTRA 9.9 repository inventory, excluding the two tables MOEX documents as removed in 9.9. It is a supported/reference inventory only: it does not assert a table-filtered request or an actual listener OPEN. CGate URLs request streams; these configurations contain no per-table filter. Exact negotiated OPEN tables remain pending.

| Service | Configured scheme policy | Supported current scheme tables | Table request | Product-consumed tables | Current-candidate OPEN |
| --- | --- | --- | --- | --- | --- |
| `FORTS_AGGR20_REPL` | `CLIENT_EXPLICIT` | orders_aggr, sys_events | The URL requests the stream; no table-list filter is configured. Exact tables delivered are pending OPEN. | orders_aggr, sys_events | `NOT_RUN_ON_CURRENT_CANDIDATE` |
| `FORTS_REFDATA_REPL` | `CLIENT_EXPLICIT` | brokers_base_contracts_params, clearing_members, dealer, discrete_auction, discrete_auction_base_contract, fut_bond_isin, fut_bond_nkd, fut_bond_nominal, fut_bond_registry, fut_exec_orders, fut_instruments, fut_margin_type, fut_sess_contents, fut_settlement_account, fut_vcb, instr2matching_map, investor, multileg_dict, opt_exp_orders, opt_sess_contents, opt_vcb, rates, sess_option_series, session, sma_master, sma_pre_trade_check, sys_events, sys_messages, trade_periods, user | The URL requests the stream; no table-list filter is configured. Exact tables delivered are pending OPEN. | session, fut_instruments, fut_vcb, fut_sess_contents, opt_sess_contents, multileg_dict, instr2matching_map, sys_messages | `NOT_RUN_ON_CURRENT_CANDIDATE` |
| `FORTS_SESSIONSTATE_REPL` | `SERVER_DEFAULT` | session_state, sys_events | Whole stream; table-level filter absent; actual OPEN pending. | session_state | `NOT_RUN_ON_CURRENT_CANDIDATE` |
| `FORTS_INSTRUMENTSTATE_REPL` | `SERVER_DEFAULT` | instrument_state, sys_events | Whole stream; table-level filter absent; actual OPEN pending. | instrument_state | `NOT_RUN_ON_CURRENT_CANDIDATE` |
| `FORTS_TRADE_REPL` | `CLIENT_EXPLICIT` | heartbeat, multileg_orders_log, orders_log, sys_events, user_deal, user_multileg_deal | Whole stream; table-level filter absent; actual OPEN pending. | heartbeat, multileg_orders_log, orders_log, sys_events, user_deal, user_multileg_deal | `NOT_RUN_ON_CURRENT_CANDIDATE` |
| `FORTS_USERORDERBOOK_REPL` | `CLIENT_EXPLICIT` | info, info_currentday, multileg_orders, multileg_orders_currentday, orders, orders_currentday | Whole stream; table-level filter absent; actual OPEN pending. | info, info_currentday, multileg_orders, multileg_orders_currentday, orders, orders_currentday | `NOT_RUN_ON_CURRENT_CANDIDATE` |
| `FORTS_POS_REPL` | `CLIENT_EXPLICIT` | info, position, position_sa, sys_events | Whole stream; table-level filter absent; actual OPEN pending. | info, position | `NOT_RUN_ON_CURRENT_CANDIDATE` |
| `FORTS_PART_REPL` | `CLIENT_EXPLICIT` | part, part_sa, sys_events | Whole stream; table-level filter absent; actual OPEN pending. | part, sys_events | `NOT_RUN_ON_CURRENT_CANDIDATE` |

### Historical compatibility names

`FORTS_REFDATA_REPL.fut_intercl_info` and `FORTS_REFDATA_REPL.opt_intercl_info` are **historical compatibility-only** for this draft. These names remain in the repository's generated/runtime compatibility inventory. The official SPECTRA 9.9 documentation says the deprecated tables were removed. Do not treat a repository lock entry as a current stream request, consumed product table, or actual server OPEN receipt.

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
- Automated test/evidence: GitHub Actions run 35445864502: connector-validation SUCCESS; component-sanitizers SUCCESS at the exact recorded head
- Live evidence: CI is not T1 live evidence

### 1b — Текущая версия

- Proposed answer: No release version assigned. Review baseline source head and exact CI run are recorded above; no deployable Linux binary hash is claimed.
- Status: `user-input`
- Source: `cert/AGGR_CERT_MATRIX_9_9.md`
- Automated test/evidence: GitHub Actions run 35445864502: connector-validation SUCCESS; component-sanitizers SUCCESS at the exact recorded head
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

- Proposed answer: Connector uses CGate to receive SPECTRA market/reference/private/status data. The current demonstration is read-only DTC/AGGR; AddOrder and DelOrder exist in a separate TEST trading path but are not live-qualified here. FullOrderLog is separate/deferred.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`, `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
- Automated test/evidence: plaza2_test_trade_transport_tests; plaza2_order_lifecycle_scenarios_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `cert/AGGR_CERT_MATRIX_9_9.md`
- Automated test/evidence: GitHub Actions run 35445864502: connector-validation SUCCESS; component-sanitizers SUCCESS at the exact recorded head
- Live evidence: CI is not T1 live evidence

### 1g — Предусмотрен ли GUI

- Proposed answer: Kairos is an external demonstration client/UI, not the certificate target. This read-only demonstration does not prove an order-entry UI.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 1h — Клиентские подключения к ВПТС

- Proposed answer: Yes: the DTC read-only server accepts one active loopback client at a time; it is separate from the CGate router connection. Commercial distribution/customer count is unresolved.
- Status: `implemented`
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`, `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1i — Работа с RFS

- Proposed answer: No for the present SPECTRA derivatives scope; no RFS stream is configured.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 1j — Аутентификация/авторизация

- Proposed answer: CGate TEST credentials/software key are deployment inputs. The read-only local DTC interface and any optional local logon are not venue/account authorization. Mandatory deployment policy and future account-level authorization require separate user/product decisions.
- Status: `user-input`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`, `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test; connector_host_test
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

- Proposed answer: User to confirm intended use and distinguish TEST qualification from any production use; no production-use claim is made.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required
- Source: `cert/AGGR_CERT_MATRIX_9_9.md`
- Automated test/evidence: GitHub Actions run 35445864502: connector-validation SUCCESS; component-sanitizers SUCCESS at the exact recorded head
- Live evidence: CI is not T1 live evidence

### 1m — Клиенты Московской Биржи, планирующие использовать ВПТС

- Proposed answer: User to identify actual organizations, if any; do not infer from software clients or repository users.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1n — Цель использования рыночных данных

- Proposed answer: User to select own trading, internal analytics/back-office, or redistribution. No redistribution answer is inferred.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1o — Язык взаимодействия с Plaza-2

- Proposed answer: C++20 is the direct CGate/native interaction layer. Rust/C# downstream consumers do not change the direct Plaza-2 language.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 1p — Распространение

- Proposed answer: Private/public noncommercial/public commercial is a user/legal decision; leave all choices unresolved.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required

### 1q — Поддерживаемые рынки MOEX

- Proposed answer: Proposed certification scope: SPECTRA derivatives. Do not check ASTS or RFS based on unrelated source artifacts.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 1r — Режимы: утренняя/дневная/вечерняя

- Proposed answer: Intended session coverage and submission checkboxes remain user/product decisions. Complete session-transition and live evidence is not established by this review.
- Status: `user-input`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_USER_OWNED_INFORMATION
- Live evidence: NOT_RUN; user answer required
- Source: `cert/AGGR_CERT_MATRIX_9_9.md`
- Automated test/evidence: GitHub Actions run 35445864502: connector-validation SUCCESS; component-sanitizers SUCCESS at the exact recorded head
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

- Proposed answer: Render sanitized effective settings from the final deployment manifest. Current code uses one local p2tcp CGate connection and eight listener URLs; DTC is a separate loopback protocol, not a Plaza connection. Do not include private endpoint/credentials.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.ii — Максимальное количество соединений

- Proposed answer: Configured ConnectorHost profile: one CGate connection object, eight replication listeners. The separate DTC server has one active client slot. Do not report eight listeners as eight router connections.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`, `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.iii — Тип соединения TCP/LRPC

- Proposed answer: TCP (p2tcp) is configured; LRPC is not configured or claimed.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.iv — Предназначение соединения

- Proposed answer: Replication/reference/status/private market data on the read path; publisher/reply and order commands belong only to the separate trading profile, not the current read-only demonstration.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`, `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
- Automated test/evidence: plaza2_test_trade_transport_tests; plaza2_order_lifecycle_scenarios_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.stream.01 — FORTS_TRADE_REPL checkbox

- Proposed answer: Selected in the eight-stream ConnectorHost profile; own order/trade reconciliation, not public tape.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.stream.02 — FORTS_COMMON_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.03 — FORTS_VM_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
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
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_authority_probe.cpp`
- Automated test/evidence: AGGR20 offline/fixture tests; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.stream.06 — FORTS_VOLAT_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.07 — FORTS_DEALS_REPL checkbox

- Proposed answer: Not selected; the current AGGR read-only product path does not use this separate public trade stream.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.08 — FORTS_POS_REPL checkbox

- Proposed answer: Selected in the eight-stream profile; not part of the four-stream day observer.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.stream.09 — FORTS_RISKINFOBLACK_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.10 — FORTS_FEE_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.11 — FORTS_PART_REPL checkbox

- Proposed answer: Selected in the eight-stream profile.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.stream.12 — FORTS_RISKINFOBACH_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.13 — FORTS_FEERATE_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.14 — FORTS_REFDATA_REPL checkbox (first printed entry)

- Proposed answer: Selected in both profiles; configured with explicit client scheme alias REFDATA.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`
- Automated test/evidence: connector_host_test effective-profile guard; plaza2_scheme_drift_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; no negotiated listener OPEN receipt
- Source: `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`, `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`
- Automated test/evidence: runtime scheme-lock checks; connector_host_test and AGGR fixture tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; exact delivered table inventory pending OPEN

### 2a.stream.15 — FORTS_INFO_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.16 — FORTS_BROKER_FEE_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.17 — FORTS_MISCINFO_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.18 — FORTS_TNPENALTY_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.19 — FORTS_BROKER_FEE_PARAMS_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.20 — FORTS_MM_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.21 — MOEX_RATES_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.22 — FORTS_USERORDERBOOK_REPL checkbox

- Proposed answer: Selected in the eight-stream profile. The configured OrdBook alias is not yet proven equivalent to the current USERORDERBOOK layout; retain that limitation and do not claim trading-profile qualification.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`
- Automated test/evidence: connector_host_test effective-profile guard; plaza2_scheme_drift_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; no negotiated listener OPEN receipt
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2a.stream.23 — FORTS_CLR_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.24 — FORTS_FORECASTIM_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 2a.stream.25 — FORTS_ORDBOOK_REPL checkbox

- Proposed answer: Not selected. This is not the configured FORTS_USERORDERBOOK_REPL service; do not conflate names.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`
- Automated test/evidence: connector_host_test effective-profile guard; plaza2_scheme_drift_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; no negotiated listener OPEN receipt

### 2a.stream.26 — RTS_INDEX_REPL checkbox

- Proposed answer: Not selected in the current profile.
- Status: `not-in-scope`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
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

- Proposed answer: Preserved as printed; same REFDATA stream as entry 14, not a second listener or separate table request.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`, `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`
- Automated test/evidence: runtime scheme-lock checks; connector_host_test and AGGR fixture tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; exact delivered table inventory pending OPEN

### 2b.I — Number of CGate-calling threads

- Proposed answer: One owner thread/processes CGate objects for the configured session; downstream DTC/client workers do not call CGate.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2b.II — Threads and purposes

- Proposed answer: The CGate owner polls connections/listeners, receives callbacks, updates state and (only in the separate armed trading profile) sends commands. The current observer and DTC read-only boundary do not publish.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`, `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2b.III — conn_process cadence

- Proposed answer: Do not claim a rate from sleep intervals. Measured calls/second, maximum gap and callback budget in idle/active/recovery remain NOT_RUN for this candidate.
- Status: `planned`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2b.IV — CGate objects used from multiple threads

- Proposed answer: No in the single-owner design; retain thread-owner assertions and do not share native CGate objects across workers.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2c.I.1 — Replication stream names and initialization URLs

- Proposed answer: Use the eight-stream inventory above for the full ConnectorHost profile and the separately identified four-stream observer profile. Status streams are declared under ‘Другие потоки’. Exact deployed endpoint values remain deployment-specific and must be sanitized.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`
- Automated test/evidence: connector_host_test effective-profile guard; plaza2_scheme_drift_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; no negotiated listener OPEN receipt
- Source: `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`, `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`
- Automated test/evidence: runtime scheme-lock checks; connector_host_test and AGGR fixture tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; exact delivered table inventory pending OPEN

### 2c.I.2 — Open/subscription mode per stream

- Proposed answer: The four-stream observer explicitly calls listener.open(mode=snapshot+online). The eight-stream profile currently has empty configured open_settings; record the exact mode from each future OPEN receipt, not by inference. TRADE may reopen from a POS replstate anchor.
- Status: `implemented`
- Source: `apps/plaza2_day_observer.cpp`, `apps/plaza2_day_observer_profile.hpp`, `connectors/connector_host/src/operator_config.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: connector_host_test; plaza2_day_observer_fixture
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2c.I.3 — Are stream data preserved between process runs

- Proposed answer: Live projected state is in memory and is rebuilt from a new stream open/snapshot after process restart. Diagnostic archives and durable order lifecycle journals are separate and are not authoritative resumable replicas of every stream.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2c.II — Handled CGate messages

- Proposed answer: The normalized callback handles OPEN, CLOSE, TRANSACTION_BEGIN, TRANSACTION_COMMIT, STREAM_DATA, ONLINE, LIFENUM, CLEARDELETED, REPLSTATE and TIMEOUT; exact native events remain part of the per-candidate receipt.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2c.III — Message interpretation method

- Proposed answer: Hybrid: runtime scheme is analyzed to resolve validated field layouts; mapped values feed the projector. Bounded raw/independent forensic decoding is used only for selected diagnostic rows. Do not claim that every message is mapped as a fixed packed struct.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`
- Automated test/evidence: plaza2_scheme_drift_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2d.I — Commands sent to the trading system

- Proposed answer: Only AddOrder and DelOrder are implemented in the separate armed TEST trading path. The current read-only observer/DTC runner has no publisher/order surface. Other listed commands are not claimed.
- Status: `implemented`
- Source: `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`, `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
- Automated test/evidence: plaza2_test_trade_transport_tests; plaza2_order_lifecycle_scenarios_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`, `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2d.II — Supported reply messages

- Proposed answer: AddOrder business reply 179 and DelOrder business reply 177 are distinct from system replies 99/100. Do not treat system 99/100 as ordinary command success. Current-candidate T1 replies: NOT_RUN.
- Status: `implemented`
- Source: `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`, `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
- Automated test/evidence: plaza2_test_trade_transport_tests; plaza2_order_lifecycle_scenarios_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 2d.III — Message allocation and destruction

- Proposed answer: The native publisher allocates each command with cg_pub_msgnew, posts it, then frees it with cg_pub_msgfree; the publisher handle is separately closed/destroyed. Read-only mode does not create the publisher.
- Status: `implemented`
- Source: `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`, `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
- Automated test/evidence: plaza2_test_trade_transport_tests; plaza2_order_lifecycle_scenarios_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`, `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 3a.I — Trading-stream heartbeats

- Proposed answer: TRADE heartbeat rows are consumed for stream health/time in the full profile. No claim is made for heartbeat support on unconfigured streams.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`, `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`
- Automated test/evidence: runtime scheme-lock checks; connector_host_test and AGGR fixture tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; exact delivered table inventory pending OPEN

### 3a.II — sys_events support

- Proposed answer: Yes for consumed AGGR20, TRADE and PART event rows, with generation/session/revision-aware handling. SESSIONSTATE and INSTRUMENTSTATE use their current-state tables; their sys_events tables are not represented as consumed product tables here.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `connectors/connector_host/src/connector_host.cpp`
- Automated test/evidence: connector_host_test; private-state and recovery tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_authority_probe.cpp`
- Automated test/evidence: AGGR20 offline/fixture tests; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json`, `protocols/plaza2_cgate/src/plaza2_private_state.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`
- Automated test/evidence: runtime scheme-lock checks; connector_host_test and AGGR fixture tests
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE; exact delivered table inventory pending OPEN

### 3a.III — Index values

- Proposed answer: No index stream is configured or claimed.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 3a.IV — Market information

- Proposed answer: Aggregated order book: yes, AGGR20. COMMONS: no. Full order log: deferred separate phase. Do not check all options merely because the form lists them.
- Status: `implemented`
- Source: `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp`, `protocols/plaza2_cgate/src/plaza2_aggr20_authority_probe.cpp`
- Automated test/evidence: AGGR20 offline/fixture tests; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN

### 3b.I — Calendar-spread trading

- Proposed answer: No calendar-spread trading capability is claimed; decoding multi-leg records alone is not spread order support.
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

- Proposed answer: No venue-side client-management command is exposed. This is distinct from downstream DTC authentication/authorization.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN
- Source: `connectors/connector_host/src/dtc_read_only_server.cpp`, `connectors/connector_host/src/dtc_market_data.cpp`, `apps/moex_connector_host_dtc_runner.cpp`
- Automated test/evidence: connector_host_dtc_protocol_test; connector_host_dtc_server_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

### 4 — Cancel on disconnect / CODHeartbeat

- Proposed answer: No qualified venue COD/CODHeartbeat function is claimed. Local socket disconnect cleanup or later manual Cancel is not COD.
- Status: `not-in-scope`
- Source: None; user-owned answer.
- Automated test/evidence: NOT_APPLICABLE_NO_IMPLEMENTED_PRODUCT_SURFACE
- Live evidence: NOT_RUN
- Source: `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp`, `protocols/plaza2_cgate/src/plaza2_runtime.cpp`, `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
- Automated test/evidence: plaza2_test_trade_transport_tests; plaza2_order_lifecycle_scenarios_test; connector_host_test
- Live evidence: NOT_RUN_ON_CURRENT_CANDIDATE

## Remaining limits before submission or trading-profile qualification

- Capture the exact current-candidate OPEN and negotiated scheme for each configured listener, especially both server-scheme status streams.
- Resolve the `OrdBook` versus USERORDERBOOK layout mismatch from an exact OPEN/client-scheme receipt before claiming eight-stream trading-profile qualification. Do not block the four-stream read-only observer on that private-stream question.
- Confirm legal identity, certificate holder, release version, business use, distribution, contacts, intended sessions, and consent with the user.
- Do not infer `conn_process` cadence from sleeps; collect a measured active/idle/recovery sample.
- No exchange order, support email, automated questionnaire submission, or live T1 claim is part of this draft.

Historical references: [MOEX CGate client manual — data scheme policy](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/Game/docs/cgate_en.pdf); [MOEX SPECTRA 9.9 gateway documentation](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/p2gate_en.html).

Machine-readable source: `docs/review/moex_cgate_questionnaire_register_9_9_20260919.json`.
