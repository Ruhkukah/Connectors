# PLAZA II TEST T1 qualification evidence

This compact bundle records the PR #28 read-only qualification after the
listener scheme-selection correction. It is intentionally redacted: no
credentials, software key, router/auth configuration, host address, or raw
stream capture is included.

The repository default for all eight typed listeners is the server-negotiated
bare URL `p2repl://<service>`. The repository matrix shows all eight listeners
created, opened, decoded data, reached `ONLINE`, completed a snapshot, and
reported replication state. The frozen vendor matrix is retained for a
qualitative comparison only; row counts are expected to vary by run time.

The combined qualifier opened all five private streams, both status streams,
AGGR20, `p2mqreply`, and the open-only publisher without allocating or posting
a publisher message. `connectivity_ready` and `publisher_ready` are true.
The final persisted receipt remains fail-closed after the target refdata and
status observations were invalidated by a late clear boundary; the bounded
status probe recorded `public_state=0`, AGGR20 was not two-sided for the
target, and no exact POS row was present. Therefore `market_state_ready`,
`account_state_ready`, and the informational `add_order_qualified` flag are
false. No order was submitted.

The runtime fingerprint is SPECTRA93 / CGate 9.3.0.1687 with zero fatal and
183 warning-only scheme-drift findings. The prior `32776 / 0x8008 /
DB:WRONG_DB_SCHEME` result is retained in `redacted_failure_before_fix.txt`.
The T0 service was unavailable; T1 remained active and no router/auth state
was changed.
