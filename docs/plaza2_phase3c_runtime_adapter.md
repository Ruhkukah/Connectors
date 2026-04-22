# Phase 3C PLAZA II CGate Runtime Adapter

## Scope

Phase 3C adds only the native runtime boundary needed before any live
replication work:

- a thin `dlopen`-based CGate adapter for environment, connection, and listener
  lifecycle
- runtime layout probing for library, scheme, and config discovery
- explicit scheme-drift detection against the Phase 3B reviewed metadata
- focused offline tests and docs

## Non-goals

- no live connectivity
- no TEST or PROD session bring-up
- no replication callbacks or state projection
- no fake replication engine
- no resume/persistence engine
- no health model
- no C ABI changes
- no `plaza2_trade`

## Runtime Adapter Architecture

The public surface is intentionally small and explicit:

- `Plaza2Settings`
- `Plaza2RuntimeProbe`
- `Plaza2Env`
- `Plaza2Connection`
- `Plaza2Listener`
- `Plaza2Error`

Vendor CGate types do not appear above this boundary. The adapter loads the
runtime dynamically and resolves only the lifecycle symbols required for Phase
3C:

- `cg_env_open`
- `cg_env_close`
- `cg_conn_new`
- `cg_conn_destroy`
- `cg_conn_open`
- `cg_conn_close`
- `cg_conn_process`
- `cg_conn_getstate`
- `cg_lsn_new`
- `cg_lsn_destroy`
- `cg_lsn_open`
- `cg_lsn_close`
- `cg_lsn_getstate`

No business logic, replication projection, reconnect policy, or background
threading is mixed into this layer.

## Runtime-Probe Architecture

`Plaza2RuntimeProbe` validates a candidate runtime root by resolving:

- a CGate shared library under `runtime_root/bin`, `runtime_root/lib`, or
  `runtime_root`
- a scheme directory under `runtime_root/scheme`, `runtime_root/Scheme`,
  `runtime_root/ini`, or `runtime_root`
- a config directory under `runtime_root/config`, `runtime_root/cfg`,
  `runtime_root/ini`, or `runtime_root`

The probe then:

- loads the runtime library and checks required symbols
- checks the environment-specific config filenames locked in Phase 3A
- parses `forts_scheme.ini` version markers where available
- computes a SHA-256 digest of the runtime-visible `forts_scheme.ini`
- compares the runtime table/field surface against the Phase 3B reviewed
  metadata

Diagnostics are surfaced as explicit probe issues instead of a generic
pass/fail.

## Scheme-Drift Rationale

Phase 3B intentionally used a reviewed `.ini` fixture derived from the locked
docs because the repo does not yet commit a locked installed `forts_scheme.ini`.

That means later phases cannot safely assume the local runtime scheme matches
the reviewed baseline. Phase 3C therefore adds explicit drift detection before
any live or stateful work depends on the installed runtime surface.

The drift probe reports at least:

- missing runtime `forts_scheme.ini`
- file hash mismatch when an expected hash is supplied
- reviewed table families missing from the runtime scheme
- reviewed table signature mismatch for matching table names
- unexpected runtime table families/signatures
- unsupported spectra release marker when an expected release is supplied

This is detection only. No runtime decode semantics or replication logic are
implemented here.

## CI / Runtime Dependency Model

Normal CI remains offline-safe:

- no installed vendor runtime is required
- no external connectivity is required
- the runtime probe and adapter are validated against a fake shared library and
  synthesized runtime layout fixtures

The adapter uses dynamic loading, so the main build does not hard-link against a
real CGate installation.

## Tests Added

- `plaza2_runtime_probe_test`
  - compatible runtime-layout fixture
  - missing config file detection
  - unexpected config file detection
  - symbol/load validation through a fake runtime library
- `plaza2_scheme_drift_test`
  - hash mismatch reporting
  - version mismatch reporting
  - reviewed-vs-runtime table drift reporting
  - unexpected runtime table reporting
  - malformed runtime scheme rejection
- `plaza2_runtime_adapter_test`
  - environment open/close
  - connection create/open/process/close/destroy
  - listener create/open/close/destroy
  - timeout handling and runtime error translation

## Known Limitations

- the adapter is lifecycle-only and does not expose callback-boundary business
  processing yet
- runtime compatibility is based on `forts_scheme.ini` table/field comparison,
  not on full live listener scheme introspection
- no Phase 3C code opens live endpoints or validates broker reachability
- installed config hashes are not yet repo-locked; only filename/layout checks
  are enforced in this phase

## Why This Still Fits A 2–4 vCPU VPS

The design remains appropriate for the later colocated target because it stays
boring:

- one thin dynamically loaded API table
- no hidden threads
- no worker pool
- no reflection-heavy registries
- no hot-path allocation strategy committed here
- schema interpretation stays mostly offline, with runtime drift checks limited
  to start-up probing

That keeps the future runtime path compatible with a lean single-threaded CGate
event loop plus, at most, one auxiliary thread in later phases.

## Next Recommended Phase

Proceed to Phase 3D: deterministic fake replication engine.
