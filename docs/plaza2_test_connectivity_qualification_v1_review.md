# PLAZA II TEST connectivity qualification V1

This PR is a target-specific, read-only qualification path for the MOEX
PLAZA II TEST environment. It remains separate from `Plaza2TestTradeTransport`,
which is fake-only. No order payload, publisher message, or cleanup command is
available on this path.

## Safety boundary

TEST network, TEST session, PLAZA II, and market-data arms are explicit. The
qualifier may create, open, inspect, and close the qualification listeners and
the open-only publisher. It does not allocate a publisher message or call
`cg_pub_post`; no live TEST order was sent.

The receipt records per-listener creation/open/ONLINE/snapshot state, the
runtime error fields, factual readiness booleans, and the individual causes
when a conjunction is false. A missing POS row remains fail-closed and is not
interpreted as zero.

## Negotiated listener schemes

The default live settings are the eight bare server-negotiated URLs:

```text
p2repl://FORTS_TRADE_REPL
p2repl://FORTS_USERORDERBOOK_REPL
p2repl://FORTS_POS_REPL
p2repl://FORTS_PART_REPL
p2repl://FORTS_REFDATA_REPL
p2repl://FORTS_SESSIONSTATE_REPL
p2repl://FORTS_INSTRUMENTSTATE_REPL
p2repl://FORTS_AGGR20_REPL
```

An explicit `--stream-settings NAME=VALUE` remains supported as an advanced
override and is recorded as `listener_url_mode=explicit_override`; omitted
known streams are recorded as `negotiated`. Settings and credentials are not
written to the receipt.

The runtime plan now binds reviewed fields to their actual negotiated ordinal
while tolerating additive runtime fields. Per-stream LifeNum tracking avoids
mistaking independent service lifetimes for a global reset. `CLEARDELETED` is
staged at the next transaction boundary (or applied before ONLINE when no
transaction follows), and refdata resets preserve already observed status
rows until the corresponding refdata rows arrive. These changes keep the
read-side projector commit-bounded and fail-closed without changing the
verified CGate ABI layout.

## Runtime and readiness gates

`Plaza2RuntimeProbe` records the installed library fingerprint, version
markers, resolved symbols, scheme SHA-256, and scheme-drift classification.
`full_version_certified` remains false without a separate ABI and scheme
review.

`market_state_ready` requires target refdata and current session membership,
`SESSIONSTATE.public_state == 1`, `INSTRUMENTSTATE.public_state == 1`, and a
fresh two-sided AGGR20 BBO. `account_state_ready` requires one exact client
PART row with `limits_set=true` and one exact client POS row with the expected
account type. `add_order_qualified` is informational only and never
authorizes an order.

## T1 evidence

The committed redacted bundle is:

`docs/evidence/plaza2_test_t1_qualification_20260825/`

It contains the source/runtime fingerprints, frozen vendor matrix, corrected
repository matrix, control-plane probe, the redacted qualifier receipt and
hash, the pre-fix wrong-scheme evidence, and T0 status. It contains no auth
file, credential, software key, router configuration, host address, or raw
capture.

### Official vendor read-only matrix

The official MOEX `basic/repl.c` sample was run one stream at a time with bare
settings and a bounded stop. All eight streams created/opened, delivered data,
reached `ONLINE`, completed a snapshot, and reported replication state:

| Stream | Rows | LifeNum | ReplState | Elapsed (ms) |
| --- | ---: | ---: | ---: | ---: |
| `FORTS_TRADE_REPL` | 78520 | 2 | 1 | 8011 |
| `FORTS_USERORDERBOOK_REPL` | 2 | 2 | 1 | 8009 |
| `FORTS_POS_REPL` | 1 | 2 | 1 | 8010 |
| `FORTS_PART_REPL` | 44 | 2 | 1 | 8010 |
| `FORTS_REFDATA_REPL` | 30346 | 2 | 1 | 8014 |
| `FORTS_SESSIONSTATE_REPL` | 45 | 2 | 1 | 8010 |
| `FORTS_INSTRUMENTSTATE_REPL` | 22868 | 2 | 1 | 8010 |
| `FORTS_AGGR20_REPL` | 31632 | 2 | 1 | 8011 |

### Corrected repository matrix

The repository implementation was rerun one stream at a time with the same
bare/default selection. All eight rows in the committed repository matrix
have `created=opened=open_event=first_data=ever_online=snapshot_complete=1`
and `error_code=runtime_code=0`. Counts are qualitative, not an equality
claim, because the runs occurred at different times.

### Combined qualifier result

The bounded run used `qualification_timeout_ms=60000` and `max_polls=0` with
target `RIU6` (ISIN id 3822999, session 11692) and participant `FZ0001o`.
All eight typed streams, `p2mqreply`, and the open-only publisher were open;
the receipt records `connectivity_ready=true` and `publisher_ready=true`.

The run correctly stopped without a readiness claim:

| Field | Result |
| --- | --- |
| `connectivity_ready` | `true` |
| `market_state_ready` | `false` |
| `account_state_ready` | `false` |
| `publisher_ready` | `true` |
| `add_order_qualified` | `false` |

The final persisted receipt records a fail-closed late-clear boundary: target
refdata and status are unavailable at the end of the bounded run. The direct
status probe recorded target session/instrument `public_state=0`; no two-sided
fresh AGGR20 BBO or exact POS row was present. The PART row is unique and has
`limits_set=true`. This is a qualification no-go for the current
market/account state, not a listener-connectivity failure.

### Control plane and T0

A temporary official-sample derivative created/opened/observed/closed
`p2mqreply` and the publisher, with zero `cg_pub_msgnew` and zero
`cg_pub_post` calls. T0 was inactive and recorded as `T0_NOT_AVAILABLE`; T1
remained active. No router or authentication configuration was changed.

### Prior connector defect

The pre-fix repository PART/REFDATA aliases returned `32776 / 0x8008 /
DB:WRONG_DB_SCHEME`, while the official vendor bare/default subscriptions
succeeded. The redacted failure file retains the affected listeners and
runtime/scheme fingerprints. The corrected repository matrix demonstrates the
connector-side scheme-selection fix.

## Validation

| Check | Result |
| --- | ---: |
| Full CTest | 157/157 pass |
| Changed-target ASan/UBSan set | 4/4 pass |
| Source style check | pass |
| .NET ABI smoke/policy | pass |
| Structural no-send/privacy checks | pass |
| `git diff --check` | pass |
| Live orders or publisher messages | none |

No new target, profile, wrapper, script, executable, router setting, or live
order path was added.
