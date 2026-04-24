# Phase 3F PLAZA II TEST Bring-Up

## Scope

Phase 3F adds the first explicit external TEST bring-up path for
`plaza2_repl`, limited to the private-state profile.

This phase adds only:

- a test-only external profile surface for `plaza2_repl`
- explicit operator arming for external TEST bring-up
- a thin single-threaded `Plaza2LiveSessionRunner`
- runtime layout probing and scheme-drift validation before listener open
- private-stream listener wiring into the Phase 3E private-state projector
- structured in-memory health snapshots and redacted operator logs
- offline-safe tests for gating, refusal paths, and fake-runtime bring-up

## Non-Goals

Phase 3F does not add:

- production connectivity or tracked production endpoints
- public aggregated or ordlog streams
- persistence or replstate journaling
- C ABI exports
- AlorEngine integration
- TWIME reconciliation
- `plaza2_trade`
- background threads, worker pools, or async frameworks

## Safety Model

External TEST bring-up is blocked unless all three explicit arm flags are
present:

- `--armed-test-network`
- `--armed-test-session`
- `--armed-test-plaza2`

`Plaza2ManualOperatorGate` enforces:

- `--armed-test-plaza2` for any Phase 3F TEST runner use
- `--armed-test-network` before non-loopback transport connect
- `--armed-test-session` before non-loopback session start

Tracked profiles remain placeholder-only or localhost-safe. Real runtime roots,
router paths, auth files, and secrets stay external and untracked.

## Arm-Flag Requirements

The user-facing wrapper is:

- `build/apps/moex_plaza2_cert_runner`

The compiled runner is:

- `build/apps/moex_plaza2_test_runner`

Typical external TEST bring-up requires:

```bash
build/apps/moex_plaza2_cert_runner \
  --profile /path/to/local/test_plaza2_repl_live_session.yaml \
  --output-dir build/test-output/plaza2-phase3f \
  --armed-test-network \
  --armed-test-session \
  --armed-test-plaza2
```

## Config / Profile Model

Tracked profile files added in this phase:

- `profiles/test_plaza2_repl_live_session.template.yaml`
- `profiles/test_plaza2_repl_live_session_local.example.yaml`

Profile section:

- `plaza2_repl_test.endpoint`
- `plaza2_repl_test.runtime`
- `plaza2_repl_test.credentials`
- `plaza2_repl_test.session`
- `plaza2_repl_test.listeners`

The wrapper flattens that YAML into explicit CLI arguments for the compiled
runner. No live defaults are embedded in code.

Credential handling is narrow:

- env or file credential sources are supported
- `${MOEX_PLAZA2_TEST_CREDENTIALS}` can be expanded into explicit CGate settings
- raw credential values are never written to logs or summary files

## Startup Validation Order

`Plaza2LiveSessionRunner::start()` performs, in order:

1. config shape validation
2. manual arm validation
3. credential loading and `${MOEX_PLAZA2_TEST_CREDENTIALS}` substitution
4. runtime layout probe
5. required symbol validation
6. required TEST config-file validation
7. scheme-drift validation
8. environment open
9. connection create/open
10. private listener create/open

The event loop remains explicit:

- `poll_once()` calls `cg_conn_process(...)`
- listener callbacks decode numeric metadata through the Phase 3C adapter
- decoded listener events are bridged into the Phase 3E projector

No background threads are started.

## Runtime / Drift Validation Behavior

Bring-up fails safely if:

- the runtime root is missing
- the CGate library is missing or lacks required symbols
- the resolved config directory is incomplete
- `forts_scheme.ini` is missing or malformed
- the runtime scheme drifts from the Phase 3B reviewed baseline
- the expected spectra release or scheme hash mismatches

The runner uses the existing Phase 3C `Plaza2RuntimeProbe` and does not bypass
scheme-drift checks.

## Health Snapshot Surface

`Plaza2LiveHealthSnapshot` exposes:

- runner state
- runtime probe status
- scheme-drift status
- last process runtime code
- last error
- last resync reason
- committed connector health from the projector
- committed resume markers (`lifenum`, `replstate`)
- per-stream create/open/online/snapshot-complete state
- committed counts for:
  - sessions
  - instruments
  - matching-map rows
  - limits
  - positions
  - own orders
  - own trades

Operator logs remain intentionally small and omit raw settings strings.

## Tests Added

Phase 3F tests added:

- `profile_check_plaza2_repl_live_session_template`
- `profile_check_plaza2_repl_live_session_local_example`
- `profile_check_plaza2_repl_fixture_real_host_fails`
- `plaza2_manual_operator_gate_test`
- `plaza2_live_session_validation_test`
- `plaza2_live_session_runner_test`
- `plaza2_cert_runner_requires_test_plaza2_arm`

These cover:

- tracked profile safety
- manual arm gating
- refusal on incompatible scheme drift
- refusal on missing credential file
- deterministic fake-runtime startup
- committed private-state counts after live-like replay
- credential redaction in operator logs

Normal CI remains offline-safe because all success-path tests run against the
fake CGate shared library fixture.

## Known Limitations

- Phase 3F is TEST-only and does not add production bring-up.
- Reconnect is intentionally not implemented yet.
- Credential substitution is limited to `${MOEX_PLAZA2_TEST_CREDENTIALS}` with the legacy
  `${PLAZA2_TEST_CREDENTIALS}` token accepted only for old local overlays.
- No on-disk resume persistence is added in this phase.
- Listener lifecycle is still private-state-only; no public streams are opened.

## Manual TEST Bring-Up Example

Use a local untracked profile derived from the template with real local paths
and TEST router values substituted outside the repository:

```bash
build/apps/moex_plaza2_cert_runner \
  --profile /path/to/local/test_plaza2_repl_live_session.yaml \
  --output-dir build/test-output/plaza2-phase3f \
  --armed-test-network \
  --armed-test-session \
  --armed-test-plaza2
```

The local profile should contain only operator-supplied paths such as:

- `runtime.root: /redacted/local/plaza2/runtime`
- `credentials.file_path: /redacted/local/auth/credential.txt`
- `session.connection_settings: p2tcp://test-router-placeholder:4001;...`

Do not commit those values.

## Why This Still Fits A 2–4 vCPU VPS

The Phase 3F design stays compatible with the later colocated target because it
is deliberately boring:

- one explicit `cg_conn_process(...)` loop
- one adapter callback path
- no thread pools
- no hidden async runtime
- numeric stream/table/field routing reused from earlier phases
- the heavy schema work remains offline in Phase 3B

That leaves headroom for the private-state process to coexist with other
components on a 2–4 vCPU host.

## Next Recommended Phase

Proceed to Phase 3G: optional lightweight public aggregated market data, while
keeping the private-state TEST bring-up path unchanged as the default base
profile.
