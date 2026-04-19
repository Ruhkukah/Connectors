# TWIME Phase 2D TCP Transport

## Scope

Phase 2D adds a real OS-level TCP byte transport for TWIME tests, but only in
the narrowest possible form:

- nonblocking POSIX TCP sockets
- hard localhost-only endpoint validation in the transport itself
- local loopback TCP server support for tests
- reconnect/backoff gating for explicit reconnect attempts
- transport metrics and deterministic transport events
- `TwimeSession` integration through the existing `TwimeFrameAssembler`

## Explicit Non-Goals

Phase 2D does **not** add:

- real MOEX connectivity
- broker connectivity
- production endpoint profiles
- credentials
- live C ABI order routing
- live AlorEngine routing
- automatic non-loopback access
- background reconnect threads

## Transport Abstraction

`ITwimeByteTransport` remains the shared session-facing abstraction for:

- `TwimeLoopbackTransport`
- `TwimeScriptedTransport`
- `TwimeTcpTransport`

`TwimeTcpTransport` is a nonblocking byte transport that surfaces deterministic
results for:

- open started / open succeeded / open failed
- partial read / partial write
- read would block / write would block
- remote close / local close
- reconnect suppression
- hard transport faults

## Local-Only Safety Model

Phase 2D keeps transport targets hard local-only in the transport itself.

- `TwimeTcpTransport` accepts only these exact hosts:
  - `127.0.0.1`
  - `::1`
  - `localhost`
- checked-in TCP profiles use only those loopback values
- any other host is rejected with a local-only violation, even if
  `allow_non_loopback=true` or `allow_non_localhost_dns=true`
- the forward-compatible override flags stay in config structs, but they are
  intentionally inert in Phase 2D and are reserved for later phases
- `TwimeTcpTransport` rejects non-test environments

This means the transport skeleton can be exercised in CI and local development
without risking accidental exchange or broker connectivity.

The exact-host rule is intentionally strict in this phase. For example:

- `127.0.0.2` is blocked
- `::ffff:127.0.0.1` is blocked
- `example.com` is blocked
- `foo.local` is blocked

Phase 2E later added a separate gated path for explicitly armed external test
endpoints. See `docs/twime_phase2e_test_endpoint_gating.md` for that safety
model. The Phase 2D document still describes the original local-only baseline.

## Loopback TCP Server Model

`tests/support/local_tcp_server.*` provides a tiny local server used only in
tests. It can:

- bind to an ephemeral localhost port
- accept one client
- send scripted byte chunks
- receive exact client payloads
- close the client deterministically

It has no MOEX-specific protocol logic. It only moves bytes.

## Session Integration

`TwimeSession` still owns TWIME session semantics while the transport remains a
byte pipe. The TCP path follows the same structure as the fake-byte path:

1. `TwimeTcpTransport` opens a local nonblocking socket.
2. `TwimeSession` polls the transport.
3. `TwimeFrameAssembler` reassembles fragmented or batched frames.
4. `TwimeSession` decodes messages and applies the existing fake/session FSM.

This keeps raw socket behavior separate from TWIME state-machine behavior.

## Reconnect and Timing Model

Phase 2D adds only an explicit reconnect policy object:

- minimum reconnect delay defaults to `1000ms`
- no jitter
- no exponential backoff
- no background reconnect loop

This models the TWIME guidance that reconnect attempts must not be more
frequent than once per second, while staying local and deterministic.

## Metrics

The TCP transport exposes stable, testable metrics including:

- `open_calls`
- `successful_open_events`
- `open_failed_events`
- `close_calls`
- `local_close_events`
- `read_calls`
- `write_calls`
- `bytes_read`
- `bytes_written`
- `partial_read_events`
- `partial_write_events`
- `read_would_block_events`
- `write_would_block_events`
- `remote_close_events`
- `reconnect_suppressed_events`
- `fault_events`
- buffer high-water metrics where buffering applies

## Why This Still Does Not Connect to MOEX

Phase 2D is intentionally not an exchange connector yet. It adds only the
socket-facing skeleton required to prove that the TWIME session code can run
over a real byte stream.

The repo still excludes:

- live exchange IPs/ports
- broker endpoints
- credentials
- production profiles
- live order routing

Real exchange-facing transport remains deferred to a later phase. Phase 2D does
not contain any MOEX or broker endpoint support, even behind config overrides.
