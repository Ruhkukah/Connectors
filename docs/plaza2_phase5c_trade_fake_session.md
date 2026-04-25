# Phase 5C PLAZA II Trade Fake Session

## Scope
Phase 5C adds a deterministic offline fake transactional session for the PLAZA II trading path.
It connects the Phase 5A command inventory, Phase 5B offline codec, Phase 3D fake replication engine, and Phase 3E
private-state projector. The fake session simulates order command outcomes and emits fake private replication
confirmation batches that can be replayed through existing commit semantics.

## Non-Goals
This phase does not add live order sending, TEST order sending, production order sending, CGate publisher lifecycle,
`cg_pub_*`, P2MQRouter submission, write-side C ABI, AlorEngine write-side integration, strategy logic, persistence,
production profiles, public market data, AGGR20, ORDLOG, or DEALS streams.

## Fake Session State Model
`Plaza2TradeFakeSession` supports fake-only states:

- `Disconnected`
- `Established`
- `Recovering`
- `Terminated`

Command submission is rejected unless the fake session is `Established`.

## Command Outcome Model
The fake session returns explicit deterministic outcomes:

- `Accepted`
- `Rejected`
- `DuplicateClientTransactionId`
- `UnknownOrder`
- `InvalidState`
- `UnsupportedCommand`
- `ValidationFailed`

Each result includes the command family, synthetic correlation id, reply message id, diagnostic reason, optional
synthetic order id, optional synthetic deal id, decoded fake reply, encoded offline command, and fake replication batch.

## Fake Order Model
Fake orders are in-memory only and include:

- synthetic order id
- client transaction id / external id
- instrument id
- side
- price
- quantity
- remaining quantity
- client code
- status: active, canceled, moved, filled, or rejected
- original command family
- last command family

The fake order book is test-only state. It is not an exchange truth source.

## Command Behavior
`AddOrder` and `IcebergAddOrder` validate through the Phase 5B codec, reject duplicate client transaction ids, generate
synthetic order ids, and emit fake confirmation rows.

`DelOrder` and `IcebergDelOrder` cancel active fake orders, reject unknown orders, reject terminal orders, and emit
cancellation confirmation rows.

`MoveOrder` and `IcebergMoveOrder` update active fake orders, reject unknown or terminal orders, and emit move
confirmation rows.

`DelUserOrders` cancels matching active fake orders by client and instrument filters represented in Phase 5B metadata.

`DelOrdersByBFLimit` fails closed as `UnsupportedCommand` because the Phase 5A/5B locked field surface contains only
`broker_code`, which is insufficient for deterministic fake cancellation semantics.

`CODHeartbeat` accepts deterministically and does not mutate order state.

## Replication Confirmation Bridge
Accepted order-state outcomes emit `Plaza2TradeFakeReplicationBatch`, which exposes a normal Phase 3D
`ScenarioDataView`.

The batch uses existing fake replication event types and streams:

- `FORTS_TRADE_REPL.orders_log`
- `FORTS_USERORDERBOOK_REPL.orders_currentday`
- `FORTS_TRADE_REPL.user_deal` for fake fill scenarios

No second fake replication engine is introduced.

## Commit-Boundary Behavior
Fake confirmations are emitted inside:

- `OPEN`
- `SNAPSHOT_BEGIN`
- `SNAPSHOT_END`
- `ONLINE`
- `TN_BEGIN`
- `STREAM_DATA`
- `TN_COMMIT`

Tests prove rows are staged before `TN_COMMIT` and become visible through `Plaza2PrivateStateProjector` only after the
commit callback.

## Correlation Model
Correlation is deterministic and exact. The fake session links:

- submitted command
- client transaction id / `ext_id`
- synthetic order id
- fake reply
- fake replication rows
- committed private-state snapshots

No fuzzy matching is used.

## Scenario Fixtures
Lightweight deterministic scenario specs live under `tests/plaza2_trade/scenarios/`.
They document the fake flows for add, reject, duplicate, cancel, move, mass cancel, heartbeat, and fake fill cases.
The tests use C++ fixtures directly so Phase 5C does not add another scenario parser.

## Tests Added
Phase 5C adds:

- `plaza2_trade_fake_session_test`
- `plaza2_trade_fake_session_replication_test`

Existing no-send tests were extended to scan the fake session source and module CMake.

The tests cover invalid-state rejection, add accept, duplicate rejection, unknown cancel rejection, cancel, move, mass
cancel, heartbeat, all command family representation, fake fill projection, commit-bounded visibility, rejected-command
non-mutation, private-state projection from fake confirmations, and no live publisher/runtime linkage.

## Offline-Only Safety
The fake session links to the offline codec and fake/private-state read-side components only.
It does not link CGate runtime or publisher code. It does not open sockets, use P2MQRouter, require credentials, require a
VPS, or produce live evidence artifacts.

## Public Market Data Note
Current live evidence validated private replication only. Public market data remains untested.
The next phase should test `FORTS_AGGR20_REPL` first. Full `FORTS_ORDLOG_REPL` should remain optional and heavy.
`FORTS_DEALS_REPL` can be added separately for anonymous trades.

## Known Limitations
The fake session is deterministic and intentionally simple. It does not model exchange risk checks, all rejection codes,
queue priority, matching-engine behavior, or live CGate publisher callbacks.

Rejected order replication surfaces remain deferred until the repo chooses a specific fake rejected-order projection path.

## Next Recommended Phase
Next phase: `Phase 5D - PLAZA II public market-data TEST bring-up, AGGR20 first`.
Public market data should be validated before live TEST order-entry. Live order sending is not the immediate next phase.
