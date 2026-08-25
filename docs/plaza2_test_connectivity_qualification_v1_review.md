# PLAZA II TEST connectivity qualification V1

This PR adds a target-specific, read-only qualification path for the real
MOEX PLAZA II TEST environment. It is intentionally separate from
`Plaza2TestTradeTransport`, which remains fake-only.

## Safety boundary

`Plaza2TradeConnectivityQualifier` accepts only runtime/session/listener
configuration and a target instrument plus participant identity. It has no
order payload, price, quantity, side, user-id, plan, or send method. The
qualifier can create, open, inspect, and close the publisher, but has no path
to post a publisher message. A structural test scans the qualifier header,
implementation, and operator executable for the publisher/order submission
surface.

The operator must arm TEST network, TEST session, PLAZA II, and market-data
qualification explicitly. A qualification failure closes only the
qualification listeners and publisher; it never issues an order cleanup
command.

## Runtime and scheme gate

Before opening TEST connectivity, `Plaza2RuntimeProbe` records the installed
library fingerprint, resolved symbols, version markers, scheme SHA-256, and
scheme-drift classification. The receipt distinguishes observed runtime,
ABI-subset compatibility, scheme-subset compatibility, and
`full_version_certified=false`. No VPS upgrade or generated-binding rewrite
is implied by a newer vendor distribution.

## Evidence

The operator selects one FUTURES target and exact client-scope participant.
The redacted receipt records:

- the five private streams, SESSIONSTATE, INSTRUMENTSTATE, AGGR20,
  untyped `p2mqreply`, and publisher state;
- target `fut_sess_contents` membership, `sess_id`, session/instrument status,
  `min_step`, `trade_mode_id`, and required refdata;
- target-only two-sided AGGR20 BBO, monotonic local age, `replID`, and
  `replRev`;
- exact participant limit identity and the applicable POS row including
  `account_type` and `xpos`;
- separate `connectivity_ready`, `market_state_ready`,
  `account_state_ready`, `publisher_ready`, and informational
  `add_order_qualified` booleans.

The canonical JSON receipt is hashed and the digest is written beside it.
Secrets, raw credentials, software keys, and raw secret-containing settings
are excluded.

## Final invariant

This qualifier is a read-only capability check. `add_order_qualified` is an
informational conjunction of the four readiness gates; it is never an order
authorization and it cannot submit a publisher message. The publisher is
opened and closed only. `Plaza2TestTradeTransport` remains fake-only, and a
failed or incomplete qualification leaves no live order or cleanup action to
perform. `full_version_certified` remains `false` until a separate ABI and
scheme review certifies the installed distribution.

## Stop condition

All local validation must pass before one real TEST qualification run is
considered. That run may prove connections, listeners, state snapshots, and
publisher open/close only. It must stop after producing the redacted receipt;
no order experiment is authorized by this PR.

## Local validation

The final offline validation on AppleClang 17 / Debug completed with:

- Full CTest: 157/157 passed, including both .NET ABI checks.
- Changed-target ASan/UBSan executable tests: 4/4 passed; structural no-send
  guard: 1/1 passed.
- Native offline plan/privacy checks: 3/3 passed.
- Structural no-send guard, source/repository style, Unicode guard, and
  `git diff --check`: passed.
- No network connection, TEST session, or order activity was performed.

The real TEST qualification run is intentionally pending the operator's
target-specific runtime, scheme, router, and local secret inputs.
