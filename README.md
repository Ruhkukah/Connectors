# MoexConnector

Phase 2A repository for a standalone C++20/Linux-first MOEX connector suite.

License: MIT. See [LICENSE](LICENSE).

Security policy: do not publish credentials, broker configs, production logs, certification logs, or broker latency/topology data. See [SECURITY.md](SECURITY.md).

This repository still stops before live protocol/session implementation. The current deliverables are:

- buildable monorepo skeleton and C ABI headers
- deterministic native stub connector with synthetic replay fixtures
- explicit ABI hardening for environment arming, polling stride, and exported layout checks
- .NET SafeHandle wrapper and batch polling/low-rate callback tests
- optional AlorEngine shadow-mode replay harness against the current seam types
- offline TWIME schema inventory, deterministic metadata generation, binary codec, frame assembler, and cert-log formatter
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
- Real MOEX protocol/network/session logic is still intentionally absent.

## Phase 2A

- `protocols/twime_sbe/` builds as a standalone C++20 library with no sockets, no C ABI dependency, and no AlorEngine dependency.
- The active TWIME schema is pinned in `protocols/twime_sbe/schema/`.
- `tools/twime_schema_indexer.py` emits XML-derived inventory into `matrix/protocol_inventory/`.
- `tools/twime_codegen.py` emits deterministic generated metadata into `protocols/twime_sbe/generated/`.
- Offline fixtures cover header/primitive/message round-trips, fragmented/batched frame assembly, and certification-style decoded logs.
- Phase 2A.1 adds strict validation for enum/set tokens, optional/null/default handling, committed golden fixture checks, metadata invariant checks, and malformed-frame hardening.
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
- Phase 2A adds only offline TWIME schema/codec logic. It does not add transport, session state, credentials, or live order routing.
