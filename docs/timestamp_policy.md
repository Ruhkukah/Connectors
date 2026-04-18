# Timestamp Policy

- Monotonic timestamps are used for latency measurement.
- UTC exchange/source timestamps are used for event time.
- Event envelopes reserve fields for:
  - socket receive time
  - decode time
  - publish time
  - managed poll time

Private and public events must carry the same timestamp envelope shape so latency and ordering analysis stay profile-neutral.
