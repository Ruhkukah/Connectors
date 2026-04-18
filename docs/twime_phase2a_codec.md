# TWIME Phase 2A Offline Codec

## Scope

Phase 2A adds an **offline-only** TWIME SBE schema inventory and binary codec layer. It does **not** add any live MOEX transport or session behavior.

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
