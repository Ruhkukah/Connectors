# TWIME Phase 2B Fake-Transport Session FSM

## Scope

Phase 2B adds a deterministic TWIME session/state-machine layer on top of the Phase 2A offline codec. Phase 2B.1 is the correctness hardening pass over that fake-session layer. It is still synthetic-only.

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
- current-schema-aware session/application message classification through generated TWIME metadata
- bounded in-memory inbound/outbound journals
- in-memory recovery-state persistence for deterministic tests
- deterministic certification-style decoded logs for all inbound/outbound session traffic
- synthetic TWIME certification scenarios runnable through `apps/moex_cert_runner`
- fake-session correctness rules for keepalive range, heartbeat frequency, retransmit count, clean terminate flow, and recoverability metadata

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
- outbound client `Sequence` heartbeats encode `NextSeqNo=null`
- inbound server `Sequence` may carry `NextSeqNo` and can trigger synthetic gap detection
- `EstablishmentAck.NextSeqNo` initializes or reconciles the next expected inbound application message number
- `EstablishmentAck` does not overwrite outbound client sequence state
- acknowledged `KeepaliveInterval` must be in the `1000..60000 ms` range and becomes the active heartbeat interval
- `EstablishmentReject` transitions the session to `Rejected` and records the reject code
- `SendTerminate` emits `Terminate` and enters `Terminating`
- clean termination requires inbound `Terminate(Finished)`; peer close before that is not a clean shutdown
- fake-clock heartbeats are scheduled from the active keepalive interval rather than an independent wall-clock timer
- the fake model prevents more than three client heartbeats per second; the TWIME-recommended `600 ms` spacing is documented but not hard-enforced separately
- only inbound application-layer messages consume the inbound application sequence counter
- session-layer messages such as `EstablishmentAck`, `Sequence`, `Retransmission`, `FloodReject`, `SessionReject`, and `BusinessMessageReject` do not consume the inbound application sequence counter
- `RetransmitRequest` can be generated from explicit commands or scripted gaps
- normal fake session recovery limits `RetransmitRequest.Count` to `10`
- fake full-recovery mode limits `RetransmitRequest.Count` to `1000`
- `Retransmission` metadata can be accepted from fake transport and logged
- `FloodReject`, `SessionReject`, and `BusinessMessageReject` are surfaced as deterministic session/application events
- `BusinessMessageReject` is surfaced but marked non-recoverable in journal metadata

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

Bounded journals store deterministic copies of inbound and outbound encoded frames together with their decoded certification log lines. Each entry also carries synthetic recoverability metadata so fake-session tests can distinguish recoverable application messages from non-recoverable session messages and rejects.

## Synthetic Certification Scenarios

Synthetic TWIME scenarios live under `cert/scenarios/twime/` and are executed by the Python `moex_cert_runner`, which delegates TWIME scenarios to the native `moex_twime_cert_runner`.

Current scenario coverage:

- `session_establish.yaml`
- `session_establish_ack_sets_inbound_counter.yaml`
- `session_reject.yaml`
- `heartbeat_sequence.yaml`
- `client_sequence_heartbeat_null_nextseqno.yaml`
- `terminate.yaml`
- `terminate_requires_inbound_terminate.yaml`
- `retransmit_last5.yaml`
- `normal_retransmit_limit_10.yaml`
- `full_recovery_retransmit_limit_1000.yaml`
- `heartbeat_rate_violation.yaml`
- `flood_reject.yaml`
- `business_reject.yaml`
- `business_reject_non_recoverable.yaml`

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
- `twime_keepalive_ack_test`
- `twime_heartbeat_fake_clock_test`
- `twime_heartbeat_rate_limit_test`
- `twime_terminate_test`
- `twime_sequence_gap_test`
- `twime_message_layer_rules_test`
- `twime_retransmit_request_test`
- `twime_retransmit_limit_test`
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
