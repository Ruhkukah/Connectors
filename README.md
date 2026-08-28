# MoexConnector

Standalone C++20/Linux-first MOEX connector components with deterministic
offline tests, guarded TEST-network tooling, and a synthetic C ABI/.NET seam.

License: MIT. See [LICENSE](LICENSE).

Security policy: do not publish credentials, broker configurations, production
logs, certification logs, or broker latency/topology data. See
[SECURITY.md](SECURITY.md).

## Capability status

### Implemented

- PLAZA II CGate runtime loading, scheme validation, private replication state,
  transaction visibility, and stream invalidation handling.
- PLAZA II AGGR20 market-data TEST bring-up behind explicit operator arms.
- Offline PLAZA trade command/reply codecs and fake transactional sessions.
- A transport-neutral PLAZA order lifecycle with fail-closed AddOrder
  uncertainty, accepted-reply-ID cancellation, exact-ext recovery, atomic local
  journals, sticky evidence inconsistency, and identifier-lock retention.
- TWIME SBE codec, framing, session/recovery state, TCP transport, guarded TEST
  session runner, health, persistence, and certification scenarios.
- Synthetic native C ABI, .NET SafeHandle adapter, ABI policy tests, and optional
  AlorEngine shadow-replay harness.

### Offline-validated

- PLAZA scheme and codec layouts, fake CGate integration, private-state
  transaction semantics, order-lifecycle V2/V2.1/V2.2 scenarios, and journal
  degradation behavior.
- TWIME codec, session, retransmission, transport, gating, persistence, and
  redaction behavior.
- Generated metadata, fixtures, profiles, matrix integrity, ABI layout, and
  deterministic tool output.

### TEST-network-evidenced

- Guarded TWIME TEST-session bring-up and PLAZA replication/AGGR20 evidence
  workflows exist for explicit operator-authorized environments.
- These workflows are separate from the default offline validation path and do
  not authorize order submission.

### Not yet wired to product C ABI

- PLAZA and TWIME protocol/session components are not exposed as live order
  submission through the public product C ABI.
- The public ABI remains the synthetic replay/shadow seam.

### Not live-order ready

- No send-capable live PLAZA order transport exists; the guarded
  `LiveTestPreSend` boundary can only validate read-side state and stop before
  publisher message allocation/posting.
- No production connectivity or live application order routing is enabled.
- A future TEST-order exercise requires a separately reviewed transport,
  authoritative runtime evidence, and explicit authorization.

## Repository layout

- `protocols/`: TWIME SBE and PLAZA II CGate protocol/runtime components.
- `connectors/`: TWIME session logic, PLAZA private-state reconciliation, and
  the TEST-only PLAZA trade lifecycle with an offline and no-send live
  pre-send boundary.
- `apps/`: offline and explicitly guarded operator runners.
- `tests/`: logical CTest cases and semantic shared test runners.
- `spec-lock/`: pinned public artifacts and reviewed local scheme locks.
- `matrix/`: machine-readable protocol coverage and adapter mappings.
- `profiles/`: replay, TEST, and guarded production-validation profiles.
- `tools/`: deterministic schema, style, profile, and inventory utilities.
- `docs/`: safety boundaries, evidence reviews, and operator runbooks.

## Build and test

Python dependencies are an explicit environment responsibility. CMake never
invokes `pip` or performs package installation.

```sh
python3 -m pip install -r requirements.txt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful build options:

- `BUILD_TESTING=OFF` omits test targets.
- `MOEX_BUILD_DOTNET_TESTS=OFF` omits .NET ABI test registration.
- `MOEX_BUILD_OPERATOR_TOOLS=OFF` omits offline operator tools when tests are
  also disabled.

CTest labels support focused validation:

```sh
ctest --test-dir build -L plaza2 --output-on-failure
ctest --test-dir build -L twime --output-on-failure
ctest --test-dir build -L sanitizer --output-on-failure
ctest --test-dir build -L tooling --output-on-failure
```

Individual consolidated cases retain their historical CTest names and can also
be selected directly, for example:

```sh
build/tests/twime_session_tests --case twime_retransmission_test
ctest --test-dir build -R '^plaza2_order_lifecycle_scenarios_test$' --output-on-failure
```

To enable the optional AlorEngine shadow replay against a local checkout:

```sh
cmake -S . -B build -DMOEX_ALORENGINE_PROJECT=/path/to/AlorEngine.csproj
ctest --test-dir build -R '^dotnet_shadow_replay$' --output-on-failure
```

## Public repository boundary

The repository intentionally excludes credentials and operational trading
data. Localhost and synthetic fixtures are the default. Any external TEST
session requires explicit arms and local credentials; production connectivity
and live order flow remain outside the implemented boundary.
