# ConnectorHost live TEST DTC runner

`moex_connector_host_dtc_runner` is a bounded, TEST-only, market-data-only
owner. It polls the existing `ConnectorHost`, samples the target AGGR20 book,
and polls one loopback DTC server in that order on one thread. Kairos is only a
demonstration consumer; this runner is the Connector certification surface.

Builds from a Git checkout discover `HEAD`. Archive/VPS builds must pass an
explicit validated revision through the separate cache override, for example
`-DMOEX_SOURCE_GIT_SHA_OVERRIDE=0123456789abcdef0123456789abcdef01234567`;
CMake rejects missing or non-40-hex provenance before building the runner and
re-discovers checkout `HEAD` after later commits. The startup receipt also
contains the exact running binary SHA-256 and refuses service if that identity
cannot be resolved.

The operator may provide board/currency bindings, but they are not required
when the committed REFDATA join supplies them:

```text
moex_connector_host_dtc_runner plaza2 qualify \
  --runtime-root PATH --scheme-dir PATH --config-dir PATH \
  --env-settings-var NAME --isin-id N --session-id N \
  [--dtc-underlying-board ASTS_SECBOARD] [--dtc-currency CODE]
```

`--dtc-board` remains a legacy alias for `--dtc-underlying-board`; neither
option sets the DTC `Exchange` field.

The remaining ConnectorHost TEST options are the same as `moexctl --help`.
The DTC listener binds only `127.0.0.1` and defaults to port `11200`. Use
`--dtc-port N`, `--dtc-symbol-id N`, or `--startup-wait-ms N` (maximum
`60000`) when the operator needs a different bounded local setting.

Optional dedicated local DTC logon authentication is enabled explicitly:

```text
--require-dtc-auth \
  --dtc-username-env MOEX_PLAZA_DTC_USERNAME \
  --dtc-password-env MOEX_PLAZA_DTC_PASSWORD
```

The default environment names are `MOEX_PLAZA_DTC_USERNAME` and
`MOEX_PLAZA_DTC_PASSWORD`; custom names are accepted only inside the
`MOEX_PLAZA_DTC_*` namespace, so the runner cannot be pointed at the existing
CGate/T1 credential variables. These credentials are bounded and checked in a
fixed-size comparison; they are never copied from or substituted for the
CGate/T1 credential settings. No credential value is printed.

The strict readonly transport still requires the configured CGate software key.
It loads the CGate/T1 credential secret only when a rendered runtime,
connection, or listener setting actually contains its credential token; a
router-authenticated readonly connection therefore needs no dummy or order
credential profile. If a read-side setting uses that token, its configured
secret source remains mandatory and missing/empty values fail closed.

Safety contract:

- The runner opts into `source_mode=live_test`; DTC replay remains the default
  for existing replay fixtures and tests.
- The ConnectorHost is TEST-only and strict readonly. It does not create
  publisher or `p2mqreply` handles, and it never calls order authorization,
  order submission, account, or order surfaces. The startup receipt reports
  source/build/binary identities, runtime/scheme identity, TEST/live source
  mode, loopback endpoint, target identity and provenance, declared
  application capabilities, fully written LOGON_RESPONSE fields,
  auth-required status, and no-order/no-account flags. A completed local
  socket write does not claim the peer consumed the response.
- Polling stays on the single ConnectorHost owner thread. During warmup the
  runner takes one committed target view after `host.poll()` for readiness and
  the startup receipt, then polls DTC. In steady state the runner no longer
  takes an unused target view; a subscribed DTC poll materializes the committed
  view it needs for snapshot/revocation checks. Publisher/reply fencing reads
  only those two handle states instead of rebuilding the broad diagnostic
  snapshot on every loop. This is a source-call-count reduction, not a latency
  or throughput benchmark.
- DTC 507 uses live target description, raw `lot_volume` contract size, and
  raw `step_price_curr` currency value per increment from committed REFDATA.
  The locked schema defines `step_price_curr` as the value of the minimum
  increment in currency; for RUB contracts the value is the same as
  `step_price`. The target futures instrument joins `fut_vcb` by
  `base_contract_code`; only one committed row with a non-empty
  `base_contract_id`, `curr` (quotation currency), and `board_md` (ASTS
  `SECBOARD` underlying-board identifier) resolves the join. The raw board
  remains separately named `underlying_board`; it is never copied into the
  standard DTC `Exchange` field.
- The gateway's explicit DTC `Exchange` identifier is `MOEX_SPECTRA`. It is a
  stable identifier defined by this gateway/client contract for the supported
  SPECTRA derivatives venue; it is not claimed to be an official MOEX code or
  a value sourced from `board_md`. Kairos must use this identifier in DTC 506
  and 102 requests. The canonical immutable definition contains both this
  identifier and the distinct raw underlying board, together with the
  `fut_vcb` source stream/table, LifeNum, replRev, base-contract key, and a
  definition version independent of AGGR book-price updates. The active
  `session` row and both futures-table source rows are separately retained
  with stream/table/replRev/LifeNum provenance.
- `SecurityDefinitionResponse` mapping is deliberately limited:

  | DTC field | LiveTest mapping | Meaning / unit / missing-value policy | Source provenance |
  |---|---|---|---|
  | `RequestID` (1) | Echo 506 request | Request correlation; no source value | Request-scoped; excluded from definition identity |
  | `Symbol` (2) | Current futures `isin` | Exact code; required | Current instrument provenance and REFDATA generation |
  | `Exchange` (3) | Gateway constant `MOEX_SPECTRA` | Explicit gateway venue label; never `board_md` | Connector/client contract; not a MOEX source field |
  | `SecurityType` (4) | `FUTURE` | Only a current outright future; spread/multileg/unknown classes fail closed | Committed instrument classification and session membership provenance |
  | `Description` (5) | Current merged instrument `name` | UTF-8 text; required | `definition_source_provenance` identifies the operative committed row; both futures table provenances are retained |
  | `MinPriceIncrement` (6) | Current instrument `min_step` | Price points per minimum increment; positive finite float32 required | Committed futures instrument/session rows, REFDATA LifeNum |
  | `CurrencyValuePerIncrement` (8) | Current instrument `step_price_curr` | RUB value of minimum increment; positive finite float32 required | Committed futures terms plus matching-generation `fut_vcb` join |
  | `IsFinalMessage` (9) | `1` | Complete one-instrument response | Connector protocol behavior; independent of DTC `SymbolID` |
  | `HasMarketDepthData` (23) | Read-only source depth capability | Boolean | Application capability, distinct from current metadata readiness |
  | `Currency` (28) | `fut_vcb.curr` | Quotation currency; only the supported `RUB` path is currently mapped | Committed `FORTS_REFDATA_REPL.fut_vcb` stream/table, replRev and LifeNum |
  | `ContractSize` (29) | Current instrument `lot_volume` | Positive whole underlying-asset unit count, exactly representable as float32 | Committed futures instrument/session rows, REFDATA LifeNum |
  | `SecurityIdentifier` (33) | Current `isin_id` | Positive MOEX source identity; distinct from DTC `SymbolID` | Committed target instrument/session provenance |

  `fut_vcb.board_md` is retained as raw ASTS SECBOARD provenance in the
  internal canonical definition and startup receipt; DTC 507 has no
  underlying-board field. LiveTest omits optional price/display conversions
  and unproven economics. Replay-only constant fields are compatibility
  fixtures, not LiveTest mappings.

  All other optional 507 tags are omitted in LiveTest. The locked MOEX schema
  defines `lot_volume` as the number of underlying-asset units in the
  instrument, but the pinned DTC schema gives `ContractSize` no unit/comment;
  mapping the former to the latter is an explicit gateway interpretation that
  still needs consumer/vendor semantic confirmation. Do not extend that
  assumption to other classes or economics.
- Phase5 currently accepts only the exact `RUB` `fut_vcb.curr` path for the
  monetary meaning needed by DTC 507. Other quotation codes, including
  `USD`/`USR`, remain visible as raw source metadata but fail closed until the
  denomination relationship to `step_price_curr` is locked by primary
  evidence; they are not labeled as proven monetary-unit economics.
- `--dtc-underlying-board` (or legacy `--dtc-board`) and `--dtc-currency` are operator bindings only. A resolved
  `fut_vcb` row overrides them and the startup receipt labels the fields
  `refdata_fut_vcb`; a mismatch fences metadata. If the join is missing or
  ambiguous, the receipt says so and live TEST 506 returns DTC 509. The
  binding values are never described as authoritative REFDATA provenance.
- Missing or malformed source terms leave 506 unavailable and return DTC 509;
  the runner does not copy replay economics into live TEST or guess board,
  currency, contract size, tick value, or clock semantics.
- Source authority is separate from connectivity. A stale, incomplete, or
  revoked ConnectorHost snapshot fences the DTC client and emits revoked
  authority before any further depth. The source supplies exchange event
  provenance; the runner does not invent ingress/emit timestamps or clock
  semantics.
- DTC 145 quantity is encoded as Float32. Source integer quantities must
  round-trip exactly through that wire type; a lossy value such as
  `16,777,217` is rejected before depth publication rather than silently
  rounded. This is a conversion-safety rule, not a claim about the venue's
  maximum supported quantity.
- DTC trading, account, position, order, and order-entry requests are rejected
  and disconnected. This Phase5 boundary is intentionally separate from any
  future execution runner.

Stop with `SIGINT` or `SIGTERM`. This README describes the local runner only;
it does not authorize live VPS, router, publisher, order, or production use.
