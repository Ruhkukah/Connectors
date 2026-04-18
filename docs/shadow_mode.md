# AlorEngine Shadow Mode

Shadow mode keeps ALOR API as the primary live path while native MOEX connectors operate in read-only mode.

Comparison domains:

- order books
- public trades
- positions
- instrument mapping
- order/status transitions

Diff reports must be deterministic:

- stable ordering by connector, instrument, and source sequence
- reproducible diff identifiers
- secret redaction applied before persistence or export
