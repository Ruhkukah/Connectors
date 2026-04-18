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
