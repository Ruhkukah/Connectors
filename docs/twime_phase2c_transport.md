# TWIME Phase 2C Fake Byte-Stream Transport

## Scope

Phase 2C introduces a byte-transport abstraction under the existing fake
TWIME session layer.

This phase adds:

- `ITwimeByteTransport`
- `TwimeLoopbackTransport`
- `TwimeScriptedTransport`
- deterministic transport states, events, and metrics
- `TwimeSession` integration through `TwimeFrameAssembler`
- byte-stream tests for fragmentation, batching, partial read/write, remote
  close, local close, and fault injection

## Explicit Non-Goals

Phase 2C does **not** add:

- real TCP sockets
- MOEX connectivity
- broker connectivity
- credentials
- live C ABI order routing
- live AlorEngine routing
- production endpoint profiles

The transport layer remains fake-only.

## Transport Interface

`ITwimeByteTransport` is a pure abstract interface with no dependency on OS
sockets:

- `open()`
- `close()`
- `write(std::span<const std::byte>)`
- `poll_read(std::span<std::byte>)`
- `state()`
- `metrics()`

The result surface is explicit and deterministic:

- `Ok`
- `WouldBlock`
- `Closed`
- `RemoteClosed`
- `Fault`
- `InvalidState`
- `BufferTooSmall`

Transport states are:

- `Created`
- `Opening`
- `Open`
- `Closing`
- `Closed`
- `Faulted`

Transport events are:

- `Opened`
- `Closed`
- `BytesWritten`
- `BytesRead`
- `PartialWrite`
- `PartialRead`
- `ReadWouldBlock`
- `WriteWouldBlock`
- `RemoteClose`
- `Fault`

## Fake Transports

### TwimeLoopbackTransport

`TwimeLoopbackTransport` is a byte-perfect in-memory loopback transport.

It supports:

- exact write/read echo
- partial reads through `set_max_read_size()`
- partial writes through `set_max_write_size()`
- remote close via `script_remote_close()`
- injected read and write faults
- explicit local close through `close()`

It uses bounded in-memory buffers and returns `WouldBlock` instead of
growing without limit once the configured buffered-byte limit is reached.

### TwimeScriptedTransport

`TwimeScriptedTransport` is a deterministic test harness for byte-stream
session tests.

It supports:

- scripted byte chunks
- scripted partial reads
- scripted write backpressure
- scripted remote close
- scripted read/write faults
- no wall-clock timing dependency

Read and write behavior depends only on the queued script and explicit
session polling.

## Session Integration

`TwimeSession` still supports the existing high-level `TwimeFakeTransport`
for focused FSM tests.

Phase 2C adds a second constructor that accepts `ITwimeByteTransport`.
When built on the byte-transport path:

- outbound frames are encoded exactly as before
- outbound bytes are written through the fake byte transport
- inbound bytes are fed into `TwimeFrameAssembler`
- complete TWIME frames are then decoded and processed by the same fake FSM

This means `TwimeFrameAssembler` is now exercised through the live session
path in tests, not only in standalone codec tests.

## Metrics and Buffering

Transport metrics currently include:

- `open_calls`
- `close_calls`
- `read_calls`
- `write_calls`
- `bytes_read`
- `bytes_written`
- `partial_read_events`
- `partial_write_events`
- `read_would_block_events`
- `write_would_block_events`
- `remote_close_events`
- `fault_events`
- `max_read_buffer_depth`
- `max_write_buffer_depth`

The buffered transports use bounded memory with deterministic caps. Tests
assert both the high-watermark accounting and backpressure behavior when a
buffer limit is reached.

## Validation

Phase 2C is validated by:

- standalone loopback transport tests
- partial read/write transport tests
- remote close and fault-injection tests
- `TwimeSession` byte-stream establish test
- `TwimeSession` fragmented frame test
- `TwimeSession` batched frame test
- repository-wide style and Unicode checks
- ASan/UBSan build and test pass

## Still Deferred

The following remain deferred to the next transport phase:

- real TCP implementation
- MOEX endpoint profiles
- live reconnect behavior
- live credentials
- live C ABI TWIME order routing
- live AlorEngine TWIME routing
