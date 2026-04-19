# Phase 2F: TWIME Test-Session Bring-Up

Phase 2F adds a real external TEST session bring-up path on top of the Phase 2D
TCP transport and the Phase 2E test-endpoint gate.

## Scope

- real TCP connect to an explicitly armed TEST endpoint
- session-level bring-up only: `Establish`, `EstablishmentAck`,
  `EstablishmentReject`, `Sequence`, `Terminate`
- operator-controlled start/stop
- bounded reconnect policy
- health snapshots, persistence, and cert-style logs

## Non-goals

- production connectivity
- production profiles
- checked-in credentials
- default live order flow
- live C ABI order routing
- live AlorEngine routing

## Safety model

External test sessions require both:

- `--armed-test-network`
- `--armed-test-session`

Localhost remains allowed by default for CI-safe integration tests.

## Persistence

Phase 2F persists session bring-up diagnostics only:

- sequence state
- clean shutdown flag
- last reject code
- active keepalive interval
- reconnect attempt count
- last transition time

Credentials are never persisted.
