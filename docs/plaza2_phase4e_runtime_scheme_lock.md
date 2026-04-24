# Phase 4E PLAZA II Runtime Scheme Lock

## Scope

Phase 4E adds a scoped runtime scheme lock for the installed PLAZA II TEST runtime and changes drift validation from
all-or-nothing table matching to compatibility classification for the private-state read-side evidence path.

The installed runtime scheme is `/opt/moex/cgate/scheme/latest/forts_scheme.ini` with SHA256
`cc3ab53b792eb1354b17615612abf173158e2f9dd78604bc47a121badf54b1c2`.

## Non-Goals

- No production connectivity.
- No order entry, `plaza2_trade`, strategy logic, or write-side ABI.
- No public market-data profile.
- No persistence or journaling.
- No firewall, router service, or daemon management changes.
- No global switch that bypasses scheme drift validation.

## Why Runtime Drift Exists

Phase 3B used the reviewed documentation-derived baseline. The VPS now runs the installed `SPECTRA93` package scheme.
Some non-projected tables differ from the reviewed baseline, and the first observed mismatch was
`FORTS_REFDATA_REPL.clearing_members`. That table is real reference data, but it is not consumed by the Phase 3E
private-state projector, so treating it as fatal blocked safe read-side evidence unnecessarily.

## Compatibility Model

Runtime drift now reports one of four states:

- `Compatible`: no drift for the checked surface.
- `CompatibleWithWarnings`: only nonfatal or non-material drift was found.
- `Incompatible`: required projected private-state fields are missing or materially incompatible.
- `Unknown`: validation could not establish compatibility.

`scheme_drift_ok` remains backward-compatible: it is true for `Compatible` and `CompatibleWithWarnings`, and false for
`Incompatible` and `Unknown`.

## Required Tables

The required private-state table set is:

- `FORTS_REFDATA_REPL.session`
- `FORTS_REFDATA_REPL.fut_instruments`
- `FORTS_REFDATA_REPL.opt_sess_contents`
- `FORTS_REFDATA_REPL.multileg_dict`
- `FORTS_REFDATA_REPL.instr2matching_map`
- `FORTS_TRADE_REPL.orders_log`
- `FORTS_TRADE_REPL.multileg_orders_log`
- `FORTS_TRADE_REPL.user_deal`
- `FORTS_TRADE_REPL.user_multileg_deal`
- `FORTS_TRADE_REPL.heartbeat`
- `FORTS_TRADE_REPL.sys_events`
- `FORTS_USERORDERBOOK_REPL.orders`
- `FORTS_USERORDERBOOK_REPL.multileg_orders`
- `FORTS_USERORDERBOOK_REPL.orders_currentday`
- `FORTS_USERORDERBOOK_REPL.multileg_orders_currentday`
- `FORTS_USERORDERBOOK_REPL.info`
- `FORTS_USERORDERBOOK_REPL.info_currentday`
- `FORTS_POS_REPL.position`
- `FORTS_POS_REPL.info`
- `FORTS_PART_REPL.part`
- `FORTS_PART_REPL.sys_events`

For these tables, missing fields or materially incompatible field types remain fatal. Field ordering differences and
same-width signedness changes are recorded as warnings because the runtime decoder binds fields by CGate field name and
runtime offset, not by reviewed field ordinal.

## Warning Tables

`FORTS_REFDATA_REPL.clearing_members` is explicitly warning-only for the current read-side evidence path. Other
non-projected runtime differences are recorded as warning/deferred drift and remain visible in the derived reports.

## Runtime Lock Artifacts

The runtime lock is recorded under `spec-lock/test/plaza2/runtime_scheme/`:

- `manifest.yaml` records the TEST polygon, package filename, package SHA256, runtime scheme path, scheme SHA256, and
  release marker.
- `runtime_scheme_signature.json` records derived table signatures from the installed runtime scheme.
- `runtime_vs_reviewed_diff.json` records fatal and warning drift separately.
- `runtime_scheme_lock_summary.md` summarizes compatibility and drift classes.

Raw vendor runtime files are not committed.

## Tooling

Use:

```bash
python3 tools/plaza2_runtime_scheme_lock.py \
  --runtime-scheme /opt/moex/cgate/scheme/latest/forts_scheme.ini \
  --phase3b-metadata protocols/plaza2_cgate/generated/plaza2_generated_metadata.json \
  --output spec-lock/test/plaza2/runtime_scheme
```

The tool exits nonzero only for `Incompatible`. It writes deterministic JSON and Markdown reports with sorted tables and
stable hashes.

## Profile Cleanup

`MOEX_PLAZA2_TEST_CREDENTIALS` is the canonical credential environment variable. Profile templates now use
`${MOEX_PLAZA2_TEST_CREDENTIALS}` in `env_open_settings`; the runner still accepts the legacy
`${PLAZA2_TEST_CREDENTIALS}` token for old untracked local overlays.

Stream settings that contain `scheme=|FILE|scheme/forts_scheme.ini|...` are rewritten after runtime probing to use the
resolved runtime scheme path, for example `/opt/moex/cgate/scheme/latest/forts_scheme.ini`.

## Evidence Behavior

Evidence now separates warning and fatal drift:

```json
{
  "scheme_drift_ok": "true",
  "scheme_drift_status": "CompatibleWithWarnings",
  "scheme_drift_warning_count": "1",
  "scheme_drift_fatal_count": "0"
}
```

If a required projected table is materially incompatible, the runner still fails closed with `scheme_drift_ok=false` and
`scheme_drift_status=Incompatible`.

## VPS Rerun Procedure

Use the bounded read-side evidence command:

```bash
source ~/.config/moex-connector/secrets/plaza2_test.env

run_id="$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$HOME/moex/evidence/plaza2-test/$run_id"

~/moex/connector/scripts/vps/plaza2_repl_test_evidence.sh \
  --bundle-root ~/moex/connector \
  --profile ~/.config/moex-connector/profiles/plaza2_repl_test.local.yaml \
  --secret-env-file ~/.config/moex-connector/secrets/plaza2_test.env \
  --output-dir "$HOME/moex/evidence/plaza2-test/$run_id" \
  --armed-test-network \
  --armed-test-session \
  --armed-test-plaza2 \
  --max-polls 256
```

Expected Phase 4E scheme result: `runtime_probe_ok=true`, `scheme_drift_ok=true`, and
`scheme_drift_status=CompatibleWithWarnings` with `clearing_members` listed as warning drift.

## Router Exposure Warning

The current P2MQRouter listens on all interfaces. Verify provider firewall or host firewall restricts access to trusted
hosts only. Phase 4E does not modify `ufw`, iptables, provider firewall, or router service state.

## Tests Added

- Scoped scheme drift tests for compatible, warning-only, and fatal cases.
- Runtime scheme lock tool fixture tests.
- Live runner tests for exact status/count exposure and stream scheme path resolution.
- Evidence script output now preserves warning/fatal separation.

## Known Limitations

- The runtime lock is TEST `t1` specific.
- Warning/deferred public or non-projected table drift is recorded, not consumed by runtime projection.
- Live listener success still depends on external CGate/router configuration after scoped scheme validation passes.

## Next Recommended Phase

Review the Phase 4E evidence. If listener open succeeds, proceed to read-side stability and operator diagnostics. If it
fails after scheme validation, classify the new failure separately instead of weakening drift checks.
