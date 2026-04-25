# PLAZA II Phase 5A Transactional Trading Spec Lock

## Scope

Phase 5A locks the PLAZA II transactional trading command surface for the practical near-term
profile:

```text
plaza2_repl + plaza2_trade
```

TWIME order-entry access is still unavailable, while the PLAZA II TEST read-side path has reached
`Ready`. This phase is therefore limited to deterministic specification inventory for the PLAZA II
CGate publisher/order-entry surface. It does not make any command runnable.

## Non-Goals

- No live, TEST, or production order sending.
- No command encoder, command builder, or publisher implementation.
- No fake transactional session.
- No write-side C ABI.
- No AlorEngine live integration.
- No public market data, `fast_md`, `fix_trade`, or TWIME changes.
- No persistence or journaling.
- No credentials, auth files, real endpoints, software keys, or evidence with secrets.

## Source Artifacts

The locked Phase 5A source surface is the installed official CGate package on the TEST VPS. Raw
vendor files are intentionally not committed. The repo commits only derived matrices and hashes.

Locked source hashes:

- `/opt/moex/cgate/docs/cgate_en.pdf`:
  `71ab9be53f8ae3c06e87598b284ee53fbec4eae3c41568870ab996592be163ca`
- `/opt/moex/cgate/docs/p2gate_en.html`:
  `ac1b5cb3d316049985a3ef40249f57f7aa07d3dec88fca1131bdb67a02d78f90`
- `/opt/moex/cgate/scheme/latest/forts_messages.ini`:
  `9ab7a54f59c9266c5d14bf8f6286cce228be1c6e7babd494909c5d3aa3f34d5b`
- `/opt/moex/cgate/scheme/latest/forts_scheme.ini`:
  `cc3ab53b792eb1354b17615612abf173158e2f9dd78604bc47a121badf54b1c2`
- `/opt/moex/cgate/samples/c/basic/send.c`:
  `405ccee59439ea87ae7ac4ef9c1d675b323313b93767a4fd75569955665ed22d`

The transactional scheme marker recorded from `forts_messages.ini` is `SPECTRA93` with DDS version
`93.1.6.39534`.

## Command Inventory

The Phase 5A command matrix is in:

```text
matrix/protocol_inventory/plaza2_trade_commands.yaml
```

Inventoried command families:

- `AddOrder`, msgid `474`: add plain futures/options order.
- `IcebergAddOrder`, msgid `475`: add iceberg order.
- `DelOrder`, msgid `461`: cancel plain order.
- `IcebergDelOrder`, msgid `464`: cancel iceberg order.
- `MoveOrder`, msgid `476`: replace or move one or two plain orders.
- `IcebergMoveOrder`, msgid `477`: replace or move iceberg order.
- `DelUserOrders`, msgid `466`: mass cancel by user/order mask.
- `DelOrdersByBFLimit`, msgid `419`: delete orders by broker-firm limit/NCC check surface.
- `CODHeartbeat`, msgid `10000`: cancel-on-disconnect heartbeat support surface.

No separate multileg order-entry command name was found in the installed `forts_messages.ini`
transactional command surface. Multileg confirmations are still mapped through existing private
replication tables where applicable.

Every command is marked:

```text
safety_classification: spec_only
runnable: false
```

## Command Fields

The field matrix is in:

```text
matrix/protocol_inventory/plaza2_trade_command_fields.yaml
```

The matrix records, per command:

- exact runtime field name
- PLAZA II type token
- stable ordinal
- required/optional classification for Phase 5 planning
- semantic role such as instrument, client, side, price, quantity, order id, or risk flag

Examples of required order-entry dependencies from the field surface:

- `broker_code`: broker/firm context
- `client_code` or `code`: client context
- `isin_id`: instrument identity used by command surfaces
- `dir` or `buy_sell`: side
- `type`: order type
- `amount`, `price`: quantity and price
- `order_id`, `order_id1`, `order_id2`: existing order identifiers for cancel/move
- `ext_id`, `ext_id1`, `ext_id2`: external/client correlation fields where present

## Reply And Error Inventory

Reply matrix:

```text
matrix/protocol_inventory/plaza2_trade_replies.yaml
```

Inventoried reply surfaces:

- `FORTS_MSG176`: `MoveOrder` result, includes `order_id1` and `order_id2`.
- `FORTS_MSG177`: `DelOrder` result, includes `amount`.
- `FORTS_MSG179`: `AddOrder` result, includes `order_id`.
- `FORTS_MSG180`: `IcebergAddOrder` result, includes `iceberg_order_id`.
- `FORTS_MSG181`: `IcebergMoveOrder` result, includes `order_id`.
- `FORTS_MSG182`: `IcebergDelOrder` result, includes `amount`.
- `FORTS_MSG186`: `DelUserOrders` result, includes `num_orders`.
- `FORTS_MSG172`: `DelOrdersByBFLimit` result, includes `num_orders`.

Error matrix:

```text
matrix/protocol_inventory/plaza2_trade_errors.yaml
```

Inventoried error surfaces:

- `FORTS_MSG99`: flood-control error with queue and penalty fields.
- `FORTS_MSG100`: generic system error with code/message fields.

Phase 5A does not implement reply or error handling logic.

## Account And Reference Dependencies

Before any later command builder can create a valid command, the following private/reference state
must already be known or deliberately supplied by an operator-local config:

- Trading session state from `FORTS_REFDATA_REPL.session`.
- Instrument identity from `FORTS_REFDATA_REPL.fut_instruments`,
  `FORTS_REFDATA_REPL.opt_sess_contents`, or `FORTS_REFDATA_REPL.multileg_dict`.
- Instrument-to-matching mapping from `FORTS_REFDATA_REPL.instr2matching_map` where later routing or
  reconciliation needs matching context.
- Client code and broker/firm context from operator config and/or private reference state.
- Limits from `FORTS_PART_REPL.part` for optional pre-check planning.
- Positions from `FORTS_POS_REPL.position` for later risk and reconciliation context.
- Active order ids from `FORTS_USERORDERBOOK_REPL.orders` and `FORTS_TRADE_REPL.orders_log` for
  cancel and move commands.
- Price step, lot size, and quantity rules from reference/instrument state before Phase 5B command
  encoding can be considered safe.

This phase inventories dependencies only. It does not implement pre-trade risk logic.

## Confirmation Mapping

Confirmation matrix:

```text
matrix/protocol_inventory/plaza2_trade_confirmation_map.yaml
```

Phase 5A maps transaction commands to existing private replication surfaces:

- Add/accept/reject:
  `FORTS_TRADE_REPL.orders_log`, `FORTS_USERORDERBOOK_REPL.orders`,
  `FORTS_REJECTEDORDERS_REPL.rejected_orders`
- Cancel/delete:
  `FORTS_TRADE_REPL.orders_log`, `FORTS_USERORDERBOOK_REPL.orders`
- Move/replace:
  `FORTS_TRADE_REPL.orders_log`, `FORTS_USERORDERBOOK_REPL.orders`
- Fills/trades resulting from orders:
  `FORTS_TRADE_REPL.user_deal`, and `FORTS_TRADE_REPL.user_multileg_deal` where applicable

The Phase 3E private-state engine already projects the core confirmation tables for orders and
trades. `FORTS_REJECTEDORDERS_REPL.rejected_orders` is present in reviewed metadata but is not part
of the Phase 3E default private projection; later phases must decide whether to add that table to
the private-state surface or handle rejections from transaction replies first.

The Phase 4A reconciler is a candidate consumer for future confirmed order/trade results, but Phase
5A does not wire it.

## Safety Classification

All Phase 5A commands remain `spec_only`.

Future safety/profile ladder:

- `plaza2_trade_offline_builder`: offline command struct/codec only; no CGate publisher.
- `plaza2_trade_fake_session`: deterministic fake publisher/reply session; no network.
- `plaza2_trade_test_order_entry`: explicitly armed TEST order-entry, bounded and operator driven.
- `plaza2_trade_test_order_entry_reconciled`: TEST order-entry fused with replicated confirmation.
- `plaza2_trade_prod_gated`: production-deferred, requires separate safety review and arming.

No profile is implemented in Phase 5A.

## Materializer And Tests

Deterministic materializer:

```text
tools/plaza2_phase5a_trade_materialize.py
```

CI check:

```bash
build/tools/plaza2_phase5a_trade_materialize --project-root . --check
```

Offline assertion test:

```text
tests/assert_plaza2_trade_spec_lock.py
```

The checks prove:

- command family coverage
- reply/error coverage
- command-field references
- confirmation map references known reviewed replication tables
- all commands remain `spec_only` and non-runnable
- generated outputs are deterministic
- no secret values are present in Phase 5A tracked outputs

## Known Uncertainties

- The installed transactional command surface does not expose a separate multileg add/delete/move
  command name. Later phases must confirm whether multileg behavior is represented through existing
  order fields, instrument definitions, or a different broker-specific surface.
- Exact enum values for side, order type, regime, masks, and compliance fields remain Phase 5B
  offline codec work.
- Exact business rejection semantics must be validated with fake-session and later armed TEST
  phases. Phase 5A only locks reply/error surfaces.
- `DelOrdersByBFLimit` is inventoried as a mass-cancel/NCC-related surface because it is present in
  `forts_messages.ini`, but it must remain deferred until operator safety requirements are explicit.

## Why No Live Order Sending

The read-side/router path is alive, but order entry needs a separate safety track: command encoding,
publisher lifecycle, transaction id/correlation, reply handling, replicated confirmation, and
operator arming. Sending even a TEST order before those are deterministic and reviewed would bypass
the repository safety model.

## Next Recommended Phase

Phase 5B - PLAZA II transactional command builder and offline codec.

Phase 5B should still be offline-only.
