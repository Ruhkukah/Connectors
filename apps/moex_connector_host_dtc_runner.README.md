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
  [--dtc-board BOARD] [--dtc-currency CODE]
```

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
  the source Git SHA, binary SHA-256, TEST/live source mode, loopback endpoint,
  target identity, authority, and no-order/no-account flags.
- DTC 507 uses live target description, raw `lot_volume` contract size, and
  raw `step_price_curr` currency value per increment from committed REFDATA.
  The locked schema defines `step_price_curr` as the value of the minimum
  increment in currency; no value is derived when it is absent. The target
  futures instrument joins `fut_vcb` by `base_contract_code`; only one
  committed row with a non-empty `base_contract_id`, `curr` (schema: quotation
  currency), and `board_md` (schema: ASTS `SECBOARD` identifier) resolves the
  join. The board string is preserved exactly; it is not converted to a
  numeric value, renamed to `FORTS`, or otherwise guessed. The ConnectorHost
  provider contract places that raw board value in the DTC 507 board field;
  this runner does not claim a separate ASTS-SECBOARD-to-DTC-exchange mapping.
- Phase5 currently accepts only the exact `RUB` `fut_vcb.curr` path for the
  monetary meaning needed by DTC 507. Other quotation codes, including
  `USD`/`USR`, remain visible as raw source metadata but fail closed until the
  denomination relationship to `step_price_curr` is locked by primary
  evidence; they are not labeled as proven monetary-unit economics.
- `--dtc-board` and `--dtc-currency` are operator bindings only. A resolved
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
- DTC trading, account, position, order, and order-entry requests are rejected
  and disconnected. This Phase5 boundary is intentionally separate from any
  future execution runner.

Stop with `SIGINT` or `SIGTERM`. This README describes the local runner only;
it does not authorize live VPS, router, publisher, order, or production use.
