# Phase 5B PLAZA II Trade Offline Codec

Phase 5B converts the Phase 5A PLAZA II transactional trading spec lock into an offline-only command construction and codec surface.
The implemented surface covers typed request structs, structural validation, deterministic payload encoding, offline reply/error decoding, generated metadata, golden fixtures, and no-send safety tests.
This phase is for the practical near-term `plaza2_repl + plaza2_trade` path while TWIME order-entry access is unavailable.

## Non-Goals
Phase 5B does not add live order sending, TEST order sending, CGate publisher lifecycle, `cg_pub_*` usage, router
submission, write-side C ABI, AlorEngine order-entry integration, production profiles, public market data, persistence, or evidence artifacts.
The codec produces bytes only. It cannot publish them.

## Source Matrices
The codec is derived from the Phase 5A locked matrices:

- `matrix/protocol_inventory/plaza2_trade_commands.yaml`
- `matrix/protocol_inventory/plaza2_trade_command_fields.yaml`
- `matrix/protocol_inventory/plaza2_trade_replies.yaml`
- `matrix/protocol_inventory/plaza2_trade_errors.yaml`
- `matrix/protocol_inventory/plaza2_trade_confirmation_map.yaml`
- `spec-lock/test/plaza2/trade/manifest.yaml`
Generated codec metadata is written to `connectors/plaza2_trade/generated/plaza2_trade_generated_metadata.json` by `tools/plaza2_phase5b_trade_codec_materialize.py`.

## Command Builders Implemented
The offline codec represents all Phase 5A command families:

- `AddOrder`
- `IcebergAddOrder`
- `DelOrder`
- `IcebergDelOrder`
- `MoveOrder`
- `IcebergMoveOrder`
- `DelUserOrders`
- `DelOrdersByBFLimit`
- `CODHeartbeat`
Each command has an explicit typed request struct in `connectors/plaza2_trade/include/moex/plaza2_trade/plaza2_trade_commands.hpp`.
No command is runnable. No command has a send, publish, submit, or router path.

## Deferred Commands
No Phase 5A command family is silently skipped.
All command families have an offline representation and structural codec coverage. Business semantics, publisher
interaction, router callbacks, fake transaction outcomes, and replication confirmation flows remain deferred.

## Validation Model
`Plaza2TradeCodec::validate` checks only structural constraints:

- required field presence
- fixed string length
- printable ASCII for fixed character fields
- valid side and order-type enums
- nonnegative or positive numeric fields where structurally required
- decimal text formatting for price fields
- active order id presence for delete and move commands
- mask and instrument fields for delete-by-mask commands
The validator does not perform pre-trade risk, limit checks, position checks, session checks, or live reference-data lookups.

## Field Encoding Model
Encoding is deterministic and locale-independent:

- `i1`, `i4`, and `i8` fields encode as little-endian signed integers.
- `cN` fields encode as fixed-width printable ASCII with zero padding.
- Price fields remain fixed-width decimal text because the locked command surface records them as `c17`.
- Missing optional numeric fields encode as zero.
- Missing optional fixed-width character fields encode as zero-filled buffers.
- Overlong strings fail validation rather than truncating silently.
The encoded output is `Plaza2TradeEncodedCommand`, containing command name, message id, payload bytes, validation status, and `offline_only=true`.

## Reply/Error Decoding Model
`Plaza2TradeCodec::decode_reply` decodes the Phase 5A locked reply/error surfaces:

- `FORTS_MSG176`
- `FORTS_MSG177`
- `FORTS_MSG179`
- `FORTS_MSG180`
- `FORTS_MSG181`
- `FORTS_MSG182`
- `FORTS_MSG186`
- `FORTS_MSG172`
- `FORTS_MSG99`
- `FORTS_MSG100`
The decoder reports message id, message name, status category, raw code fields, order id fields, queue/penalty fields, amount/count fields, and message text where present.
Unknown or short payloads fail closed with explicit validation status.

## Golden Fixtures
Golden fixtures live under `tests/plaza2_trade/fixtures/`.
The committed hex fixtures cover:

- `add_order_minimal.golden.bin.hex`
- `del_order_minimal.golden.bin.hex`
- `move_order_minimal.golden.bin.hex`
- `del_user_orders_minimal.golden.bin.hex`
- `cod_heartbeat_minimal.golden.bin.hex`
All fixture identifiers are synthetic and contain no real account codes, credentials, endpoints, or evidence data.

## No-Send Safety Model
The offline library is `moex_plaza2_trade_offline_codec`.
It does not link to CGate runtime or publisher targets. It does not reference `cg_pub_*`. It exposes no send, publish, submit, runner, profile, or CLI path.
`is_sendable` returns false for encoded commands because Phase 5B artifacts are offline-only by construction.

## Tests Added
Phase 5B adds:

- `plaza2_trade_codec_materialize_check`
- `plaza2_trade_command_validation_test`
- `plaza2_trade_command_encoding_test`
- `plaza2_trade_reply_decoding_test`
- `plaza2_trade_no_send_guard_test`
The tests cover command coverage, required-field failures, invalid enum failures, invalid price/quantity failures, fixed-string
length rejection, deterministic golden encoding, reply/error decoding, generated metadata determinism, and no-send guards.

## Known Limitations
The byte layout is an offline locked-layout model derived from Phase 5A field order and field type tokens. Live CGate
publisher integration may require an additional adapter step once official publisher runtime behavior is introduced.
Reply status categories are structural. Exact business rejection taxonomy remains a Phase 5C/5D validation item.
No fake transactional session exists yet, so command accept/reject outcomes and private-state replication confirmation are not simulated in this phase.

## Next Recommended Phase
Next phase: `Phase 5C - PLAZA II fake transactional session`.
Phase 5C should remain offline-only and simulate command submission, accept/reject replies, order creation, cancel/move outcomes, and confirmation through the existing private-state fake replication surfaces.
