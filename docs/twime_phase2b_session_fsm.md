# TWIME Phase 2B Fake-Transport Session FSM

## Scope

Phase 2B adds a deterministic TWIME session/state-machine layer on top of the Phase 2A offline codec. It is still synthetic-only.

Implemented in Phase 2B:

- `connectors/twime_trade/` standalone module
- fake transport with scripted inbound frames and peer-close events
- TWIME session states:
  - `Created`
  - `ConnectingFake`
  - `Establishing`
  - `Active`
  - `Terminating`
  - `Terminated`
  - `Rejected`
  - `Faulted`
  - `Recovering`
- fake-clock driven heartbeat scheduling through `on_timer_tick()`
- sequence tracking for outbound next sequence and expected inbound sequence
- scripted sequence-gap detection and synthetic `RetransmitRequest` generation
- bounded in-memory inbound/outbound journals
- in-memory recovery-state persistence for deterministic tests
- deterministic certification-style decoded logs for all inbound/outbound session traffic
- synthetic TWIME certification scenarios runnable through `apps/moex_cert_runner`

Explicit non-goals remain:

- no real TCP sockets
- no MOEX connectivity
- no broker endpoints
- no credentials
- no production TWIME network mode
- no public C ABI order routing
- no live AlorEngine order routing

## Module Layout

- `connectors/twime_trade/include/moex/twime_trade/twime_session.hpp`
- `connectors/twime_trade/include/moex/twime_trade/twime_fake_transport.hpp`
- `connectors/twime_trade/include/moex/twime_trade/twime_sequence_state.hpp`
- `connectors/twime_trade/include/moex/twime_trade/twime_recovery_state.hpp`
- `connectors/twime_trade/include/moex/twime_trade/twime_rate_limit_model.hpp`
- `connectors/twime_trade/include/moex/twime_trade/twime_cert_scenario_runner.hpp`

The library target is `moex_twime_trade`. It depends on `moex_twime_sbe` only. It does not depend on sockets, CGate, the managed adapter, or the public C ABI.

## Session Behavior

Phase 2B currently models these session-level messages:

- `Establish`
- `EstablishmentAck`
- `EstablishmentReject`
- `Terminate`
- `Sequence`
- `RetransmitRequest`
- `Retransmission`
- `FloodReject`
- `SessionReject`
- `BusinessMessageReject`

Modeled behavior:

- `ConnectFake` emits `Establish` from config and enters `Establishing`
- `EstablishmentAck` transitions the session to `Active`
- `EstablishmentReject` transitions the session to `Rejected` and records the reject code
- `SendTerminate` emits `Terminate` and enters `Terminating`
- fake peer close during termination transitions to `Terminated`
- `Sequence` can be sent from the fake clock and received from the fake peer
- sequence gaps move the session into `Recovering`
- `RetransmitRequest` can be generated from explicit commands or scripted gaps
- `Retransmission` metadata can be accepted from fake transport and logged
- `FloodReject`, `SessionReject`, and `BusinessMessageReject` are surfaced as deterministic session/application events

## Fake Clock and Timers

There are no wall-clock timers in Phase 2B.

Time is modeled with:

```cpp
TwimeFakeClock clock;
clock.advance(milliseconds);
session.on_timer_tick();
```

Heartbeat scheduling uses fake-clock milliseconds only. This keeps the tests deterministic and avoids any thread or timer dependency.

## Recovery and Journals

Phase 2B adds durable-state interfaces but uses only in-memory/test implementations:

- `next_outbound_seq`
- `next_expected_inbound_seq`
- `last_establishment_id`
- `recovery_epoch`
- `last_clean_shutdown`

Bounded journals store deterministic copies of inbound and outbound encoded frames together with their decoded certification log lines. They are designed for tests and scenario replay, not for performance.

## Synthetic Certification Scenarios

Synthetic TWIME scenarios live under `cert/scenarios/twime/` and are executed by the Python `moex_cert_runner`, which delegates TWIME scenarios to the native `moex_twime_cert_runner`.

Current scenario coverage:

- `session_establish.yaml`
- `session_reject.yaml`
- `heartbeat_sequence.yaml`
- `terminate.yaml`
- `retransmit_last5.yaml`
- `flood_reject.yaml`
- `business_reject.yaml`

Run one directly:

```sh
build/apps/moex_cert_runner \
  --scenario cert/scenarios/twime/session_establish.yaml \
  --output-dir build/twime-cert-runner
```

## Deterministic Logs

All inbound and outbound session messages are written as decoded certification-style lines using the current TWIME schema metadata.

The log format is intentionally deterministic:

- no local absolute paths
- no wall-clock timestamps added by the logger
- only schema-derived message/field rendering
- stable ordering matching fake transport input/output order

## Test Surface

Phase 2B adds:

- `twime_session_establish_test`
- `twime_session_reject_test`
- `twime_heartbeat_fake_clock_test`
- `twime_terminate_test`
- `twime_sequence_gap_test`
- `twime_retransmit_request_test`
- `twime_retransmission_test`
- `twime_flood_reject_test`
- `twime_business_reject_test`
- `twime_cert_scenario_runner_test`

Run the Phase 2B subset:

```sh
ctest --test-dir build --output-on-failure -R "twime_.*(session|heartbeat|terminate|sequence|retransmit|flood|business|cert_scenario)"
```

## Not Live Trading Ready

Phase 2B is not certification-ready and must not be used for live trading.

Still missing:

- real TCP transport
- real session timing/reconnect behavior
- recovery-service transport
- production sequence/retransmit cache
- credentials and endpoint handling
- public C ABI order routing
- AlorEngine live TWIME order path

Those belong in later phases.
