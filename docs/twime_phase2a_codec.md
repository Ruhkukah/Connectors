# TWIME Phase 2A Offline Codec

## Scope

Phase 2A adds an **offline-only** TWIME SBE schema inventory and binary codec layer. Phase 2A.1 hardens that layer with stricter fixture validation, explicit null/string policy, and stronger metadata/frame invariants. It does **not** add any live MOEX transport or session behavior.

Implemented in Phase 2A:

- pinned active schema at `protocols/twime_sbe/schema/twime_spectra-7.7.xml`
- pinned schema manifest with source artifact, hash, schema id, version, and byte order
- XML-derived schema inventory for TWIME messages, fields, types, and enums
- deterministic generated C++ metadata
- offline message header encode/decode
- offline primitive type encode/decode
- offline message encode/decode for the required Phase 2A TWIME subset
- byte-stream frame assembler for fragmented/batched offline buffers
- certification-style decoded text formatter for offline fixtures
- matrix coverage updates for TWIME certification scenarios at the schema/codec level
- strict enum and set-token validation for fixtures and decoded messages
- committed golden fixture integrity checking
- metadata invariant checks against the pinned schema
- bounded frame-assembler error handling with explicit buffered-state reset after fatal header errors

Explicitly **not** implemented in Phase 2A:

- TCP sockets
- connect/reconnect logic
- Establish/Terminate runtime session flow
- heartbeat timers
- session numbering/retransmission runtime state
- recovery-service runtime connection
- rate limiting/flood control runtime behavior
- order submission through the public C ABI
- AlorEngine live trading path changes
- broker/MOEX credentials or endpoint profiles

Phase 2A.1 keeps the same non-goals. There is still:

- no TCP
- no session FSM
- no recovery cache
- no live credentials
- no order routing

## Active Schema

- file: `protocols/twime_sbe/schema/twime_spectra-7.7.xml`
- schema id: `19781`
- schema version: `7`
- byte order: `littleEndian`
- package: `moex_spectra_twime`

The active schema hash is pinned in `protocols/twime_sbe/schema/schema.manifest.json`.

`twime_spectra-7.0.xml` remains a historical comparison item in `spec-lock`; it is **not** the active codec target.

## Source Of Truth

The binary layout source of truth is the locked TWIME XML schema, not historical examples or older public samples.

That means:

- message header layout comes from `messageHeader` in the XML
- block lengths come from the XML-derived fixed field sizes
- enum/set wire values come from the XML
- `Decimal5` uses the XML composite definition: encoded `mantissa`, constant exponent `-5`

If a PDF narrative or old certification example disagrees with the XML for binary layout, the XML wins. Any discrepancy should be tracked as documentation-only `needs_confirmation` in `matrix/gaps.yaml`.

## Fixture Policy

Golden TWIME fixtures are committed inputs and outputs:

- source fixtures: `tests/fixtures/twime_sbe/*.yaml`
- expected binary outputs: `tests/fixtures/twime_sbe/expected/*.hex`
- expected certification-style logs: `tests/fixtures/twime_sbe/expected/*.certlog`

Normal test runs and CI must treat the committed `.hex` and `.certlog` files as read-only goldens. They are verified with:

```sh
build/tools/twime_fixture_check --fixtures tests/fixtures/twime_sbe --check
```

If a schema-driven change legitimately requires a golden update, regenerate intentionally with:

```sh
build/tools/twime_fixture_check --fixtures tests/fixtures/twime_sbe --write
```

At least six fixtures contain manual-review comments describing why the expected hex is stable:

- `Establish`
- `Sequence`
- `NewOrderSingle`
- `OrderCancelRequest`
- `OrderReplaceRequest`
- `ExecutionSingleReport`

Those reviewed goldens are additionally asserted by an independent Python check so that expected values are not derived only from the same C++ encoder under test.

## Null, Decimal, and String Policy

Optional field handling comes from the pinned XML schema:

- omitted nullable fields encode their XML-defined null value
- omitted required fields fail with `TwimeDecodeError::InvalidFieldValue`
- `TimeStamp` null is encoded as `uint64 max`
- `Decimal5` carries an `int64` mantissa only; the exponent is a constant `-5` and is not encoded separately
- nullable `Decimal5` fields use the XML null mantissa where applicable

Fixed string policy at the wire edge:

- fixed-size char arrays are encoded without heap allocation
- short values are padded with `0x00`
- trailing `0x00` and space bytes are trimmed on decode
- embedded `0x00` bytes inside the retained decoded range are preserved
- control bytes other than embedded `0x00` are rejected as `TwimeDecodeError::InvalidStringEncoding`
- overlength strings fail instead of truncating silently

Enum and bitset/set policy:

- unknown enum token names fail with `TwimeDecodeError::InvalidEnumValue`
- unknown set token names fail with `TwimeDecodeError::InvalidSetValue`
- mixed valid and invalid set-token lists fail
- raw set masks with unknown bits fail

## Error Policy

Phase 2A.1 classifies codec and framing errors precisely but still does **not** attach runtime session decisions to them.

Errors currently distinguished include:

- `NeedMoreData`
- `BufferTooSmall`
- `InvalidBlockLength`
- `UnsupportedSchemaId`
- `UnsupportedVersion`
- `UnknownTemplateId`
- `InvalidEnumValue`
- `InvalidSetValue`
- `InvalidStringEncoding`
- `InvalidFieldValue`
- `TrailingBytes`

The frame assembler is intentionally bounded:

- invalid headers do not leave corrupted buffered state behind
- invalid schema/version/block length clears partial buffered bytes
- `max_frame_size` is enforced before allocation growth
- ready frames already assembled remain available in order

Whether any of these should later map to `Terminate`, `SessionReject`, disconnect, or retransmit behavior belongs to Phase 2B, not Phase 2A.

## Code Generation

Tools:

- `tools/twime_schema_materialize.py`
- `tools/twime_schema_indexer.py`
- `tools/twime_codegen.py`

Generation is deterministic by design:

- stable message/type/field order from the XML
- no timestamps in generated files
- no local absolute paths
- stable include order

To regenerate and verify:

```sh
build/tools/twime_schema_indexer \
  --schema protocols/twime_sbe/schema/twime_spectra-7.7.xml \
  --out matrix/protocol_inventory

build/tools/twime_codegen \
  --schema protocols/twime_sbe/schema/twime_spectra-7.7.xml \
  --out protocols/twime_sbe/generated \
  --check
```

Generated output policy:

- generated C++ must stay deterministic and readable
- committed generated files must never contain timestamps or local absolute paths
- CI verifies the checked-in output with `--check`
- if generation changes intentionally, regenerate locally and commit both the generator change and the updated generated files in the same reviewable diff

## Fixtures And Tests

Fixture roots:

- `tests/fixtures/twime_sbe/*.yaml`
- `tests/fixtures/twime_sbe/expected/*.hex`
- `tests/fixtures/twime_sbe/expected/*.certlog`

Current fixture coverage includes:

- `Establish`
- `Sequence`
- `NewOrderSingle` day/GTD
- `OrderCancelRequest`
- `OrderReplaceRequest`
- `OrderMassCancelRequest` futures/options
- `RetransmitRequest`
- `ExecutionSingleReport`
- `BusinessMessageReject`
- `SystemEvent`

Run the TWIME offline test set:

```sh
ctest --test-dir build --output-on-failure -R twime_
```

Additional hardening checks:

```sh
ctest --test-dir build --output-on-failure -R "twime_|source_style_check"
python3 tests/assert_twime_metadata_invariants.py
python3 tests/assert_twime_reviewed_fixtures.py
```

## Certification Log Formatting

Phase 2A only formats **offline decoded messages** into a certification-style text line:

```text
MessageName (blockLength=..., templateId=..., schemaId=..., version=..., Field=value, ...)
```

This matches the required style direction for certification logs, but it is **not** a full certification runner and **not** a session/runtime implementation.

## Why There Are No Sockets Yet

The goal of Phase 2A is to make the TWIME schema and binary layout reproducible before any transport or state machine work begins.

Phase 2B is where fake transport/session behavior should start:

- Establish / EstablishmentAck / EstablishmentReject runtime state
- Sequence heartbeats
- Terminate handling
- numbering/retransmission model
- synthetic cert-runner TWIME scenarios

Until then, the module remains a standalone offline C++20 codec library.

## Phase 2A.1 Status

Phase 2A.1 hardens the offline codec before merge to `main`:

- strict validation of enum and bitset tokens
- explicit optional/null/default coverage
- explicit fixed-string policy tests
- independent golden-fixture review checks
- metadata invariant checks against XML-derived inventory
- safer frame assembly under malformed input

It is still offline-only and still not suitable for live trading or certification execution.
