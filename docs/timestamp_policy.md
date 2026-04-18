# Timestamp Policy

- Monotonic timestamps are used for latency measurement.
- UTC exchange/source timestamps are used for event time.
- Event envelopes reserve fields for:
  - `socket_receive_monotonic_ns`
  - `decode_monotonic_ns`
  - `publish_monotonic_ns`
  - `managed_poll_monotonic_ns`

Private and public events must carry the same timestamp envelope shape so latency and ordering analysis stay profile-neutral.
