# PLAZA II TEST connectivity qualification V1

This PR is a target-specific, read-only qualification path for the MOEX
PLAZA II TEST environment. It is separate from `Plaza2TestTradeTransport`,
which remains fake-only. No order, publisher message, or cleanup command is
part of this path.

## Safety boundary

`Plaza2TradeConnectivityQualifier` may create, open, inspect, and close the
qualification listeners and publisher. It has no order payload, price,
quantity, side, user-id, plan, or publisher-message send method. The
qualifier's publisher is open-only. The structural no-send check remains a
required gate.

TEST network, TEST session, PLAZA II, and market-data arms are explicit. A
failed start or incomplete qualification records the failing listener and
typed runtime error details while preserving the created/opened status of
earlier listeners. Cleanup is limited to closing qualification objects.

## Runtime, scheme, and status gates

Before connectivity, `Plaza2RuntimeProbe` records the installed library
fingerprint, resolved symbols, version markers, scheme SHA-256, and
scheme-drift classification. `full_version_certified` remains false unless a
separate ABI and scheme review certifies the installed distribution.

`market_state_ready` requires all of the following factual evidence:

- `SESSIONSTATE.public_state == 1` and `INSTRUMENTSTATE.public_state == 1`;
- the raw numeric status values are retained in the receipt, together with
  `target_session_add_capable` and `target_instrument_add_capable`;
- target refdata is complete and target-only AGGR20 has a fresh, versioned,
  two-sided BBO;
- private, status, AGGR20, and untyped `p2mqreply` listeners are ready; and
- exact participant limit identity and the applicable position row are
  present (a missing POS row is not interpreted as zero).

AGGR20 evidence is epoch-bound. A changed `LifeNum`, `Close`, or
`ClearDeleted` resets the projector, clears the BBO, and makes the contour
not ready. A later `ONLINE` event cannot resurrect the prior epoch's BBO.

The primary qualification deadline is monotonic
`--qualification-timeout-ms` (default 60 seconds). `--max-polls` is retained
only as a secondary bound. The receipt contains per-listener
created/opened/online/snapshot state, the failure listener, runtime error
code, runtime error text, and the factual readiness booleans.

## T1 evidence

The redacted evidence directory is:

`/home/azgaldov/moex/evidence/plaza2-qualification-t1-20260825T114101Z`

It contains the source/runtime/scheme fingerprints, service and socket
snapshots, nonsecret profile filenames, router-resolution excerpts, the
repository run output, and the matrices below. Authentication files,
software keys, credentials, and raw router configuration were not read into
the evidence or changed.

The prior combined repository result remains **`PARTIAL_T1_CONNECTIVITY`**:
the runtime probe and scheme drift check were usable, but the repository's
scoped auxiliary listener configuration produced service-resolution errors.
This is not reported as failed market-data connectivity.

### Official vendor read-only matrix

The official MOEX `basic/repl.c` sample, used from a temporary copy and run
one stream at a time with an eight-second bounded stop, resolved, opened,
received data, reached `ONLINE`, completed a snapshot, and produced a
replication state for every required contour:

| Stream | Data rows | LifeNum | Replication state | Elapsed (ms) |
| --- | ---: | ---: | ---: | ---: |
| `FORTS_TRADE_REPL` | 78520 | 2 | 1 | 8011 |
| `FORTS_USERORDERBOOK_REPL` | 2 | 2 | 1 | 8009 |
| `FORTS_POS_REPL` | 1 | 2 | 1 | 8010 |
| `FORTS_PART_REPL` | 44 | 2 | 1 | 8010 |
| `FORTS_REFDATA_REPL` | 30346 | 2 | 1 | 8014 |
| `FORTS_SESSIONSTATE_REPL` | 45 | 2 | 1 | 8010 |
| `FORTS_INSTRUMENTSTATE_REPL` | 22868 | 2 | 1 | 8010 |
| `FORTS_AGGR20_REPL` | 31632 | 2 | 1 | 8011 |

The first PART/REFDATA attempts with the repository's scoped aliases returned
`32776/0x8008 DB:WRONG_DB_SCHEME`; the vendor default-scheme reruns succeeded.
The vendor-success/repository-failure split is therefore classified as a
**connector configuration defect**. Router configuration was not edited and
expansion stopped at this boundary.

### Control-plane probe

A temporary official-CGate-sample derivative independently created and opened
the `p2mqreply://;ref=PUB` listener and `p2mq://...;name=PUB` publisher,
observed both active, then explicitly closed the reply listener and publisher.
It contained no `cg_pub_msgnew` or `cg_pub_post` call and sent no message.

### T0

`auth/t0.ini` was checked only for non-empty existence. `cgate@t0` was
inactive, and the current T1 service remained active. Stopping T1 was not
permitted for this SSH user, so T0 was recorded as **`T0_NOT_AVAILABLE`**;
no auth or router configuration was changed. T1 remained active at the end.

## Final invariant

`add_order_qualified` is an informational conjunction of
`connectivity_ready`, `market_state_ready`, `account_state_ready`, and
`publisher_ready`. It is never an order authorization. A qualification run
may prove connectivity, listener state, snapshots, and publisher open/close,
but it cannot submit a publisher message or a TEST order. No live TEST order
or `cg_pub_post` was sent for this PR.

The local journal language used by the surrounding lifecycle work is
**“atomic local journal; no power-loss durability claimed”**; this PR does not
claim durable orphan journals.

## Local validation

With the repository Python environment selected, the changed qualifier target
builds and its focused test passes. The changed-target ASan/UBSan build and
test also pass. The native no-send/privacy checks and `git diff --check` are
run again for the final head.

The full CTest run is 156/157: all functional tests pass, while
`source_style_check` reports pre-existing AppleClang-format differences in
unchanged baseline lines of the touched files (the same check also fails on
the parent files). No broad formatting change is included in this PR.
