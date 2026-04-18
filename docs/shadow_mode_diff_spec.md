# Shadow Mode Diff Spec

Diff reports are deterministic and stable across reruns against the same input window.

Primary keys:

- profile id
- instrument
- account
- source connector
- source sequence
- recovery epoch

Diff categories:

- order book mismatch
- public trade mismatch
- position mismatch
- instrument mapping mismatch
- status transition mismatch

All diffs must include both event-time and latency-time envelopes, plus redacted source metadata.

Phase 1 synthetic replay baseline structure:

- `router`
- `low_rate_callbacks`
- `instruments`
- `order_book.snapshot`
- `order_book.updates`
- `market_data.snapshot`
- `market_data.updates`
- `order_statuses`
- `trade_executions`
- `positions`
- `health`
- `backpressure`

The committed synthetic baseline lives under `tests/fixtures/shadow_replay/expected_report.json`.
