# MoexConnector

Phase 0 repository skeleton for a standalone C++20/Linux-first MOEX connector suite.

This repository intentionally stops before protocol implementation. The current deliverables are:

- buildable monorepo skeleton and C ABI headers
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

## Definition of Done Commands

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
build/tools/spec_indexer --config spec-lock/lock_roots.json --workspace .
build/tools/matrix_validate --matrix-dir matrix
build/apps/moex_cert_runner --scenario cert/stub/phase0_stub.yaml --output-dir build/cert-runner
build/tools/profile_check --profile profiles/prod_fast_twime.template.yaml
```

The last command is expected to fail unless `--armed` is supplied.
