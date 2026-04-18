# Phase 1 Status

## Implemented

- deterministic native stub connector behind the C ABI
- synthetic replay fixture loading with no exchange sockets
- explicit environment-start arming ABI
- .NET SafeHandle wrapper around the native library
- stride-aware batch polling ABI with a v1 compatibility wrapper
- low-rate callback registration for replay-state events
- ABI size and alignment validation from managed tests
- optional AlorEngine shadow replay harness against current seam types
- deterministic shadow-mode diff report generation from synthetic events

## Still Synthetic

- all event flow comes from committed replay fixtures
- connector health/backpressure state is derived from synthetic replay state
- order, trade, position, L1, and order-book events are stub projections only
- shadow-mode comparisons validate integration behavior, not exchange correctness

## Explicitly Not Implemented

- TWIME, FIX, FAST, SIMBA, CGate, or PLAZA II transport logic
- live MOEX sessions, broker sessions, or any production network connectivity
- real order submission, cancel, replace, or mass-cancel behavior
- exchange recovery, resend, or sequence management
- authentication, broker credentials, or production topology handling

## Must Not Be Used For Live Trading

- the native connector remains synthetic and read-only
- the AlorEngine shadow adapter is an integration harness, not a trading gateway
- placeholder order-entry ABI calls still return `MOEX_RESULT_NOT_SUPPORTED`
- no part of Phase 1 / 1.1 is suitable for certification or production trading
