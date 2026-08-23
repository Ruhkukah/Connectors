# Connector core debloat V1 review

## Gate result

The refactor preserves 154 logical CTest cases while removing 40 build targets
and 53 compiled executables. The root CMake file is now orchestration-only,
configure performs no package installation or network access, and CI has one
authoritative execution of each check. No MOEX or broker connection was made
and no live TEST order was sent.

Baseline is merged `main` at
`1663e69581292982af05d8b3eda031337c7a25c1`. Measurements were taken on the
same Apple Silicon/macOS host with AppleClang 17, CMake 4.2.3, Ninja, Debug,
eight build/test workers, and already-installed local Python requirements.
Times are wall-clock observations, not benchmarks.

## Baseline and final metrics

| Measure | Baseline | Final | Change |
|---|---:|---:|---:|
| CMake/build targets (`--target help`) | 226 | 186 | -40 (-17.7%) |
| CMake File API executable targets | 97 | 44 | -53 (-54.6%) |
| Test/helper executable targets (excluding five apps and `twime_fixture_check_tool`) | 91 | 38 | -53 (-58.2%) |
| Logical CTest cases | 154 | 154 | unchanged |
| Root `CMakeLists.txt` LOC | 1,128 | 79 | -1,049 (-93.0%) |
| Clean configure | 0.95 s | 0.67 s | -0.28 s |
| Clean Debug build | 20.17 s | 12.81 s | -7.36 s |
| Incremental no-op build | 0.01 s | 0.01 s | unchanged |
| Full parallel CTest | 12.14 s | 11.58 s | -0.56 s |
| Sanitizer critical build | 37.85 s | 19.63 s | -18.22 s |
| Sanitizer cases / wall time | 76 / 17.77 s | 65 / 2.83 s | scoped to labeled critical cases |
| Python/shell operator wrappers | 13 | 13 | unchanged |
| Profile templates | 9 | 9 | unchanged |
| CI jobs per pull-request update | 4 duplicated executions | 2 | push/PR duplication removed |

Tracked source LOC counts physical lines in C/C++ headers/sources, C#, Python, shell, CMake files, and top-level `CMakeLists.txt`; generated build trees and Markdown are excluded.
The final diff is 1,307 additions and 1,283 deletions (net +24); the required 146-line review document accounts for that small net increase, while tracked source is a net deletion.

| Component | Baseline | Final |
|---|---:|---:|
| root CMake | 1,128 | 79 |
| adapters | 1,091 | 1,091 |
| apps | 2,999 | 3,060 |
| cmake modules | 0 | 71 |
| connectors | 11,133 | 11,141 |
| include | 987 | 987 |
| protocols | 54,310 | 54,310 |
| scripts | 1,014 | 1,014 |
| src | 1,765 | 1,765 |
| tests | 14,287 | 15,078 |
| tools | 6,972 | 6,972 |
| **Total** | **95,686** | **95,568** |

## Control-plane consolidation

- Root CMake now declares project-wide options, core libraries, component directories, and the apps/tests boundary. App and launcher wiring lives in `apps/CMakeLists.txt`; test inventory lives in `tests/CMakeLists.txt`.
- `MOEX_BUILD_DOTNET_TESTS` and `MOEX_BUILD_OPERATOR_TOOLS` complement standard `BUILD_TESTING` without a feature-flag explosion. A build with tests and operator tools disabled configures and builds without Python.
- Configure-time `pip` and the build-local dependency directory were deleted. `requirements.txt` is an explicit environment/CI responsibility, and generated launchers only inherit/prepend environment paths.
- The PLAZA offline/live boundary is structural: configuration fails if
  `moex_plaza2_trade_offline_codec` links the live CGate runtime. The retained
  behavioral no-send test checks the offline-only/non-sendable interface
  instead of grepping source layout.
- `spec_indexer_policy_assert` now depends explicitly on its manifest-producing fixture, eliminating a parallel CTest race.

## Test executable mapping

Every prior CTest name remains separately selectable with `ctest -R`. Sixty former one-case TWIME executables map one-to-one to `runner --case <original-test-name>`:

| Former executable set | Semantic runner | Cases |
|---|---|---:|
| SBE inventory, primitives, headers, round trips, framing, errors, formatting, enums, optional strings | `twime_sbe_tests` | 9 |
| session establish/reject, keepalive, heartbeat, reconnect timing, terminate, sequence, retransmit and business rules | `twime_session_tests` | 15 |
| loopback byte transport, partial I/O, remote close, faults, fragmented/batched session streams | `twime_byte_transport_tests` | 8 |
| TCP connect, partial I/O, close, faults, reconnect and loopback server | `twime_tcp_transport_tests` | 7 |
| TCP session establish, fragmentation, batching, close and terminate | `twime_tcp_session_tests` | 5 |
| endpoint resolution, credentials, network/operator gates and external-session validation | `twime_endpoint_gate_tests` | 9 |
| health, persistence, live runner/reject/timeout and reconnect policy | `twime_live_runner_tests` | 7 |

Important boundaries remain independent: PLAZA order lifecycle scenarios, fake CGate runtime integration, runtime adapter/probe, offline no-send behavior, phase-0/C ABI, and .NET ABI tests.

The logical taxonomy is expressed with CTest labels: `unit`, `plaza2`, `twime`,
`integration`, `tooling`, `preflight`, `sanitizer`, and `abi`. Tests were not
deleted merely for similarity; CTest count and names are unchanged.

## TWIME teardown finding

Five live-session fixtures created a joinable `std::thread` and joined only on
the success path. Any assertion/exception before the explicit join invoked
`std::terminate` during stack unwinding, presenting as an intermittent teardown
failure. The fixtures now use `std::jthread`, whose destructor joins safely. No
retry, delay, protocol/session change, or socket behavior change was introduced.

## CI before and after

Before: all branch pushes and pull requests each launched both jobs, so a PR
branch update commonly produced four job executions. The main job ran full
CTest and then directly repeated matrix validation, TWIME codegen, TWIME
goldens, cert stub, and profile arm checks; it also ran a non-asserting
schema-index rewrite. The sanitizer job built everything and used a name regex.

After: branch validation is authoritative through `pull_request`; `push` is
limited to `main`. Unicode/repository/source style are direct preflight and
excluded from the later CTest pass. All other checks run through CTest once.
The component sanitizer job builds only seven TWIME runners plus critical
phase-0/PLAZA boundaries and runs the explicit `sanitizer` label with fail-fast
ASan/UBSan options.

## Dead-scaffolding census

| Classification | Disposition |
|---|---|
| `KEEP_RUNTIME` | protocol/connector libraries, current apps, all runtime/test/prod profiles; wildcard profile validation covers templates without exact-name references |
| `KEEP_TEST` | fake CGate runtime, local TCP support, reviewed fixtures, cert scenarios, parser/materializer assertions, redaction and arm-gate tests |
| `KEEP_HISTORICAL_EVIDENCE` | `scripts/vps/*`, phase-4D/5D assertion scripts, and referenced bring-up/evidence docs; these retain operator reproduction value |
| `MERGE` | 60 narrow TWIME executable registrations into seven semantic runners; root app/test wiring into component CMake files |
| `DELETE` | configure-time pip machinery, duplicate launcher path entry, non-behavioral source grep in the no-send guard, duplicated CI executions, and the non-asserting CI schema-index rewrite |

No file was deleted based on phase naming or age. Historical evidence was not reorganized in this bounded PR.

## Preserved safety invariants

The refactor does not change PLAZA private-state transaction visibility,
`lifenum`/`replstate`, scheme drift checks, locked codec layouts, V2/V2.1/V2.2
lifecycle semantics, AddOrder uncertainty, the DelOrder reply-ID rule,
exact-ext DelUserOrders recovery, journal degradation/identifier locks, sticky
reply/replication evidence inconsistency, or TWIME
codec/session/retransmit/transport behavior. It adds no product C ABI
submission and no live order transport.

Validation preserves scheme/runtime/private-state tests; codec
validation/encoding/reply tests; exact-ext, P2MQ timeout, journal degradation
and lifecycle scenarios; TWIME codec/session/retransmit/transport tests;
redaction/secret gates; and native/.NET ABI tests.

## Deferred bloat and recommendation

PLAZA test executables remain comparatively granular because fake-runtime
loading, sanitizer-sensitive lifecycle boundaries, and distinct link surfaces
still provide useful isolation. Phase-era documentation and VPS evidence remain
discoverable rather than being renamed wholesale. Public phase-0 symbols and
ABI types are intentionally untouched.

**Recommendation:** merge this control-plane-only refactor after the draft
review gate, then make any live PLAZA TEST order transport a separate explicitly
authorized branch with its own connectivity, credential, arm, and evidence
plan. Do not extend this branch into live transport.
