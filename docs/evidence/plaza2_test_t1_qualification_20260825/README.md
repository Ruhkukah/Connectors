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
The `lifenum_diagnostic.txt` record resolves the apparent LifeNum discrepancy:
the official sample and repository callbacks decode the installed 9.3 header's
8-byte `cg_data_lifenum_t` payload identically, while the frozen vendor
column `2` is a different matrix metric. This is classified
`SAME_VALUE_DIFFERENT_MATRIX_METRIC`.

The combined qualifier opened all five private streams, both status streams,
AGGR20, `p2mqreply`, and the open-only publisher without allocating or posting
a publisher message. `connectivity_ready` and `publisher_ready` are true.
The final persisted receipt is internally reset and fail-closed: the target is
present in refdata, but both status rows have `public_state=0`, AGGR20 has no
two-sided target book, and no exact POS row is present. Therefore
`market_state_ready`, `account_state_ready`, and the informational
`add_order_qualified` flag are false. Its terminal classification is
`NOT_READY`, its explicit classification is
`TEST_CONTOUR_NOT_CURRENTLY_TRADEABLE`, and it includes a redacted read-only
POS census for the authorized participant. No order was submitted.

The temporary candidate census observed 330 FUTURES instruments, 327 current
session members, and zero candidates after the `SESSIONSTATE.public_state == 1`
gate (and therefore zero after every later gate). The separate POS census has
zero rows; missing POS remains **NOT PROVEN ZERO**.

The runtime fingerprint is SPECTRA93 / CGate 9.3.0.1687 with zero fatal and
183 warning-only scheme-drift findings. The prior `32776 / 0x8008 /
DB:WRONG_DB_SCHEME` result is retained in `redacted_failure_before_fix.txt`.
The T0 service was unavailable; T1 remained active and no router/auth state
was changed.

The requested rerun of the previously failed GitHub component-sanitizers job
passed, including the previously flaky TWIME case; that earlier failure was
classified as an unrelated timing flake.
