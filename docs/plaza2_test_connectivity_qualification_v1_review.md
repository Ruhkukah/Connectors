# PLAZA II TEST connectivity qualification V1.1

PR #28 is a target-specific, read-only qualification path for the MOEX
PLAZA II TEST environment. It remains separate from
`Plaza2TestTradeTransport`, which is fake-only. The PR includes the
read-only qualifier executable; it does not include an order path.

## Safety boundary

TEST network, TEST session, PLAZA II, and market-data arms are explicit. The
qualifier may create, open, inspect, and close qualification listeners and the
open-only publisher. It does not allocate a publisher message or call
`cg_pub_post`; no live TEST order was sent. Credentials, settings, router
configuration, and raw captures are excluded from committed evidence.

The receipt records per-listener creation/open/ONLINE/snapshot state, runtime
errors, readiness booleans, terminal classification, and individual failure
causes. A missing POS row remains fail-closed as **NOT PROVEN ZERO**.

## V1.1 replication invariants

`P2REPL_CLEARDELETED` is retained as a typed pending event carrying
`stream_code`, `table_code`, `table_rev`, and `flags`. It is applied at the
existing transaction-safe boundary. For a table with source revision `r`, a
clear at `R` removes only rows with `r < R`; `INT64_MAX` clears only the
addressed table. Table-scoped cleanup never calls the stream-wide projector.
Internal source-revision metadata covers the qualifier's REFDATA,
SESSIONSTATE, INSTRUMENTSTATE, POS, PART, and related order/book tables.
Uncommitted clears remain invisible until commit, and subsequent rows rebuild
the cleared table normally.

LifeNum is tracked and delivered per replication stream. A changed LifeNum
invalidates only that stream's owning projector domain; the initial value and
an identical repeat do not invalidate any domain. The verified CGate 9.3 ABI
layout was not changed.

Refresh clears every target-derived field before looking up the target. A
bounded run is persisted as terminal `READY`, `NOT_READY`, or `ERROR`; a
not-ready run is never represented by `qualification_state="Started"` as its
final interpretation.

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

## Runtime and readiness gates

`Plaza2RuntimeProbe` records the installed library fingerprint, version
markers, resolved symbols, scheme SHA-256, and scheme-drift classification.
`full_version_certified` remains false without a separate ABI and scheme
review.

`market_state_ready` requires target FUTURES refdata and current-session
membership, `SESSIONSTATE.public_state == 1`,
`INSTRUMENTSTATE.public_state == 1`, a valid min step and trade mode, and a
fresh two-sided AGGR20 BBO. `account_state_ready` requires one exact client
PART row with `limits_set=true` and one exact client POS row with the expected
account type. `add_order_qualified` is informational only and never
authorizes an order.

## T1 evidence

The committed redacted bundle is:

`docs/evidence/plaza2_test_t1_qualification_20260825/`

It contains source/runtime fingerprints, the frozen vendor matrix, the
corrected repository matrix, the LifeNum ABI diagnostic, control-plane probe,
candidate census, POS census, the redacted qualifier receipt and hash, the
pre-fix wrong-scheme evidence, and T0 status.

The official matrix column is named `vendor_lifenum_metric`; it is not the raw
CGate LifeNum payload. The repository column is named `cg_data_lifenum`.
`lifenum_diagnostic.txt` records the installed `cg_data_lifenum_t` layout,
raw bytes, decoded values, and the official `basic/repl.c` comparison. The
disposition is `SAME_VALUE_DIFFERENT_MATRIX_METRIC`; no runtime decode change
was required.

The final qualifier receipt is target-specific and read-only. It includes the
terminal classification, internally reset target fields, all readiness
booleans, and a redacted POS census for the authorized participant. The
candidate census first applies every declared market-state gate. If it is
empty, the result is classified `TEST_CONTOUR_NOT_CURRENTLY_TRADEABLE`; no
order is attempted.

The bounded T1 census observed 330 FUTURES instruments and 327 current-session
members, but zero instruments with `SESSIONSTATE.public_state == 1`; the final
qualified count was zero. The authorized-participant POS census contained no
rows, which remains **NOT PROVEN ZERO**, not an inferred zero position.

## Control plane and T0

A temporary official-sample derivative created/opened/observed/closed
`p2mqreply` and the publisher with zero `cg_pub_msgnew` and zero
`cg_pub_post` calls. T0 was inactive and recorded as `T0_NOT_AVAILABLE`; T1
remained active. No router or authentication configuration was changed.

## Validation

| Check | Result |
| --- | ---: |
| Full CTest | 157/157 pass |
| Changed-target ASan/UBSan set | 4/4 pass |
| Source style check | pass |
| Repository style check | pass |
| ABI smoke/policy | pass |
| No-send/privacy checks | pass |
| GitHub connector-validation | pass |
| Requested GitHub sanitizer rerun | pass; prior TWIME failure classified as an unrelated timing flake |
| `git diff --check` | pass |
| Live orders or publisher messages | none |

The order-lifecycle wording remains **atomic local journal; no power-loss
durability claimed**. No durable orphan-journal claim is made. No new target,
profile, wrapper, or script was added; PR #28 includes the read-only
qualifier executable.
