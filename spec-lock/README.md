# Spec Lock

`spec-lock/` stores cached official artifacts, recursive hash manifests, and local CGate scheme locks.

- `replay/`: replay-only fixtures and deterministic local captures
- `test/`: MOEX and broker test-environment artifacts
- `prod/`: production-environment artifacts

Use `build/tools/spec_indexer --config spec-lock/lock_roots.json --workspace .` to populate the remote artifact caches and manifests.
