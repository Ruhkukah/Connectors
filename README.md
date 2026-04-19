# MoexConnector

Phase 2F repository for a standalone C++20/Linux-first MOEX connector suite.

License: MIT. See [LICENSE](LICENSE).

Security policy: do not publish credentials, broker configs, production logs, certification logs, or broker latency/topology data. See [SECURITY.md](SECURITY.md).

This repository still stops before live protocol/session implementation. The current deliverables are:

- buildable monorepo skeleton and C ABI headers
- deterministic native stub connector with synthetic replay fixtures
- explicit ABI hardening for environment arming, polling stride, and exported layout checks
- .NET SafeHandle wrapper and batch polling/low-rate callback tests
- optional AlorEngine shadow-mode replay harness against the current seam types
- offline TWIME schema inventory, deterministic metadata generation, binary codec, frame assembler, and cert-log formatter
- deterministic TWIME fake-transport session FSM with synthetic certification scenarios
- TWIME test-endpoint gating, local env/file credential loading, and redacted test-runner plumbing
- TWIME external TEST session bring-up, health snapshots, persistence, and manual operator gating
- profile templates with production arming checks
- machine-readable matrix files with referential integrity
- artifact lock tooling for MOEX public specs and local CGate schemes
- certification-runner and replay-tool stubs
- AlorEngine adapter and event mapping artifacts
- shadow-mode and diff-report design docs

## Layout

- `spec-lock/`: cached official artifacts, manifests, and local scheme locks
- `matrix/`: linked machine-readable coverage and mapping files
- `profiles/`: runtime profile templates
- `docs/`: shadow mode, topology, time, and redaction policies
- `include/`: Phase 0 core headers and C ABI
- `tools/`: utility scripts copied into `build/tools/`
- `apps/`: runner stubs copied into `build/apps/`

## Status

- Phase 1 implemented a synthetic native connector, the .NET adapter, and AlorEngine shadow replay.
- Phase 1.1 hardened the ABI with explicit boolean types, clearer environment-start exports, stride-aware polling, and stronger ABI policy tests.
- Phase 2A adds an offline-only TWIME SBE schema inventory and deterministic codec generated from the pinned `twime_spectra-7.7.xml` schema.
- Phase 2A.1 hardens that offline TWIME layer with strict enum/set validation, golden fixture checks, metadata invariants, and bounded frame-assembler behavior.
- Phase 2B adds a fake-transport TWIME session state machine with deterministic journals, fake-clock heartbeat scheduling, and synthetic cert-runner scenarios.
- Phase 2B.1 hardens that fake-session layer so `Sequence`, `EstablishmentAck.NextSeqNo`, `Terminate`, keepalive handling, retransmit limits, and heartbeat-rate rules follow the current TWIME spec more closely.
- Phase 2B.2 adds message-counter reset handling, overflow-safe retransmit arithmetic, pending retransmission completion rules, reconnect timing guards, and PR-diff Unicode checks.
- Phase 2C adds a TWIME byte-transport abstraction plus loopback/scripted
  byte-stream transports. `TwimeSession` can now consume fragmented and
  batched inbound byte streams through `TwimeFrameAssembler`, while real
  sockets remain deferred.
- Phase 2D adds a local-only nonblocking TCP transport skeleton for TWIME,
  a loopback test server, reconnect/backoff gating, and TCP-backed session
  tests.
- Phase 2E adds test-endpoint gating and local-only credential plumbing for
  explicitly armed non-loopback **test** endpoints. Localhost remains the
  default, production connectivity is still blocked, and live order routing
  remains disabled by default.
- Phase 2F adds explicitly armed external **test-session** bring-up for the
  TWIME session layer only: TCP open, Establish, EstablishmentAck or
  EstablishmentReject, Sequence heartbeat, bounded reconnect, Terminate, and
  health or persistence reporting. Live application order flow remains
  disabled by default.
- Real MOEX protocol/network logic is still intentionally absent.

## Phase 2A / 2B

- `protocols/twime_sbe/` builds as a standalone C++20 library with no sockets, no C ABI dependency, and no AlorEngine dependency.
- The active TWIME schema is pinned in `protocols/twime_sbe/schema/`.
- `tools/twime_schema_indexer.py` emits XML-derived inventory into `matrix/protocol_inventory/`.
- `tools/twime_codegen.py` emits deterministic generated metadata into `protocols/twime_sbe/generated/`.
- Offline fixtures cover header/primitive/message round-trips, fragmented/batched frame assembly, and certification-style decoded logs.
- Phase 2A.1 adds strict validation for enum/set tokens, optional/null/default handling, committed golden fixture checks, metadata invariant checks, and malformed-frame hardening.
- `connectors/twime_trade/` now adds a fake-transport TWIME session FSM, in-memory recovery/journals, synthetic gap/retransmit handling, and synthetic cert-runner scenarios.
- Phase 2C keeps `TwimeFakeTransport` for high-level FSM tests and adds
  byte-stream loopback/scripted transports for deterministic
  transport/session integration tests. This exercises the frame assembler
  through the session path without introducing sockets.
- See [docs/twime_phase2c_transport.md](docs/twime_phase2c_transport.md)
  for the fake byte-stream transport model and its explicit non-goals.
- Phase 2D adds `TwimeTcpTransport` for local loopback testing only and
  keeps it behind the same session/frame-assembler path. See
  [docs/twime_phase2d_tcp_transport.md](docs/twime_phase2d_tcp_transport.md)
  for the local-only TCP model and its explicit safety restrictions.
- Phase 2E adds a second gated path for explicitly armed external **test**
  endpoints, plus local env/file credential loading and redaction. See
  [docs/twime_phase2e_test_endpoint_gating.md](docs/twime_phase2e_test_endpoint_gating.md)
  and [docs/secrets_and_redaction.md](docs/secrets_and_redaction.md).
- Phase 2F adds a second operator arm for actual external **test-session**
  bring-up and exposes session health, persistence, and reconnect
  observability. See
  [docs/twime_phase2f_test_session_bringup.md](docs/twime_phase2f_test_session_bringup.md)
  and [docs/twime_operator_runbook.md](docs/twime_operator_runbook.md).
- Phase 2B.1 corrects client `Sequence` heartbeats to encode
  `NextSeqNo=null`, treats `EstablishmentAck.NextSeqNo` as inbound state,
  requires inbound `Terminate(Finished)` for clean shutdown, validates
  acknowledged keepalive, enforces fake retransmit limits, and separates
  trading-rate vs total-rate modeling.
- This is **not** certification-ready and **not** suitable for live trading.

## Definition of Done Commands

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
build/tools/spec_indexer --config spec-lock/lock_roots.json --workspace .
build/tools/matrix_validate --matrix-dir matrix
build/tools/twime_schema_indexer --schema protocols/twime_sbe/schema/twime_spectra-7.7.xml --out matrix/protocol_inventory
build/tools/twime_codegen --schema protocols/twime_sbe/schema/twime_spectra-7.7.xml --out protocols/twime_sbe/generated --check
build/apps/moex_cert_runner --scenario cert/stub/phase0_stub.yaml --output-dir build/cert-runner
build/apps/moex_cert_runner --scenario cert/scenarios/twime/session_establish.yaml --output-dir build/twime-cert-runner
build/apps/moex_cert_runner --scenario cert/scenarios/twime/client_sequence_heartbeat_null_nextseqno.yaml --output-dir build/twime-cert-runner
build/apps/moex_cert_runner --scenario cert/scenarios/twime/terminate_requires_inbound_terminate.yaml --output-dir build/twime-cert-runner
build/apps/moex_cert_runner --profile profiles/test_twime_tcp_external_local.example.yaml --output-dir build/twime-test-gate --validate-only
build/apps/moex_cert_runner --profile profiles/test_twime_live_session_local.example.yaml --scenario cert/scenarios/twime/live_test_session_establish.yaml --output-dir build/twime-live-session --validate-only
build/tools/profile_check --profile profiles/prod_fast_twime.template.yaml
dotnet run --project tests/dotnet/AbiSmoke/AbiSmoke.csproj --framework net10.0 -- build/lib/libmoex_phase0_abi.dylib tests/fixtures/shadow_replay/synthetic_replay.txt
dotnet run --project tests/dotnet/AbiPolicy/AbiPolicy.csproj --framework net10.0 -- build/lib/libmoex_phase0_abi.dylib tests/fixtures/shadow_replay/synthetic_replay.txt
```

`build/tools/profile_check --profile profiles/prod_fast_twime.template.yaml` is expected to fail unless `--armed` is supplied.

To run the optional AlorEngine shadow replay harness against a local checkout:

```sh
cmake -S . -B build -DMOEX_ALORENGINE_PROJECT=/path/to/AlorEngine.csproj
ctest --test-dir build --output-on-failure -R dotnet_shadow_replay
```

## Public Repo Boundaries

- This repository is public and intentionally excludes operational trading data.
- Do not commit credentials, broker configs, production logs, certification logs, or broker latency/topology information.
- Phase 1 / 1.1 changes may harden the native stub, managed adapter, shadow replay, ABI policy, CI, docs, and test scaffolding, but do not add MOEX protocol logic.
- Phase 2A adds only offline TWIME schema/codec logic.
- Phase 2B adds only fake-transport TWIME session logic. It does not add real sockets, credentials, or live order routing.
- Phase 2D adds only local loopback TCP transport.
- Phase 2E adds only gated test-endpoint validation and local credential
  plumbing. It still does not add production connectivity, broker
  production connectivity, checked-in credentials, or live order routing by
  default.
- Phase 2F adds only explicitly armed external **test-session** bring-up and
  observability. It still does not add production connectivity, checked-in
  credentials, broker production routing, or live application order flow by
  default.
