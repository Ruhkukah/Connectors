# Phase 5D PLAZA II AGGR20 Market Data Bring-Up

## Scope

Phase 5D adds a read-only TEST bring-up path for the PLAZA II public aggregated order-book stream
`FORTS_AGGR20_REPL`. The phase proves runtime probing, scoped scheme-drift validation, AGGR20 listener
opening, `orders_aggr` row projection, bounded execution, and redacted evidence output.

## Non-goals

This phase does not add order entry, publisher runtime, write-side C ABI, strategy logic, production profiles,
public `FORTS_ORDLOG_REPL`, public `FORTS_ORDBOOK_REPL`, or public `FORTS_DEALS_REPL`.

## Why AGGR20 First

AGGR20 is the narrow public market-data profile already identified by the earlier PLAZA II inventory as the
default lightweight aggregated-book target. It is materially lighter than full order-log or full order-book streams
and is the right first public stream for a 2-4 vCPU colocated VPS.

MOEX configures the aggregated depth at the login/router side. Phase 5D therefore tests one configured stream:
`FORTS_AGGR20_REPL`. If the login is not entitled or configured for AGGR20, evidence must classify the failure
instead of broadening to other public streams.

## Deferred Public Streams

`FORTS_ORDLOG_REPL` and `FORTS_ORDBOOK_REPL` remain deferred because they are heavier and should be validated after
the lightweight aggregated path is stable. `FORTS_DEALS_REPL` is a separate anonymous public trades stream and is
also deferred. The Phase 5D validator rejects these streams.

## Profile Model

Tracked profiles are:

- `profiles/test_plaza2_aggr20_md.template.yaml`
- `profiles/test_plaza2_aggr20_md_local.example.yaml`

Both are TEST-only and contain placeholders or localhost-safe values only. They require:

- `--armed-test-network`
- `--armed-test-session`
- `--armed-test-plaza2`
- `--armed-test-market-data`

The only allowed stream is `FORTS_AGGR20_REPL`.

## Runner Model

The runner surface is `Plaza2Aggr20MdRunner`, exposed through:

- `apps/plaza2_aggr20_md_test_runner.cpp`
- `apps/moex_plaza2_aggr20_md_runner.py`
- `scripts/vps/plaza2_aggr20_md_test_evidence.sh`

Startup order:

1. Validate TEST-only profile and all arm flags.
2. Validate that only `FORTS_AGGR20_REPL` is configured.
3. Load credentials through the existing redacted credential provider.
4. Run runtime probe and scoped scheme-drift validation.
5. Open CGate environment and connection.
6. Open one AGGR20 listener.
7. Process bounded polls.
8. Project committed `orders_aggr` rows.
9. Emit redacted evidence and stop.

## Projection Model

`Plaza2Aggr20BookProjector` stages rows inside `TN_BEGIN` / `TN_COMMIT` and publishes only committed snapshots.

Projected fields:

- `isin_id`
- `price`
- `volume`
- `dir`
- `replID`
- `replRev`
- `moment`
- `moment_ns`
- `synth_volume`

Snapshot fields:

- row count
- instrument count
- bid depth levels
- ask depth levels
- top bid
- top ask
- last repl/revision markers
- stream online and snapshot-complete flags

`dir=1` is treated as bid and `dir=2` as ask for the minimal AGGR20 snapshot.

## Evidence Artifacts

The VPS evidence script writes:

- `startup.json`
- `runtime_probe.json`
- `scheme_drift.json`
- `readiness.json`
- `final_health.json`
- `aggr20_snapshot.json`
- `operator.log`
- `run_manifest.json`

Artifacts redact credentials and do not include passwords, software keys, auth file contents, or secret env contents.

## VPS Runbook

Use an untracked local profile on the VPS:

```bash
cp ~/moex/connector/profiles/test_plaza2_aggr20_md.template.yaml \
   ~/.config/moex-connector/profiles/plaza2_aggr20_md.local.yaml
chmod 600 ~/.config/moex-connector/profiles/plaza2_aggr20_md.local.yaml
```

Edit only local runtime paths and connection settings. Do not put raw passwords into the profile.

Run a bounded evidence campaign:

```bash
source ~/.config/moex-connector/secrets/plaza2_test.env

run_id="$(date -u +%Y%m%dT%H%M%SZ)-aggr20"
mkdir -p "$HOME/moex/evidence/plaza2-md/$run_id"

~/moex/connector/scripts/vps/plaza2_aggr20_md_test_evidence.sh \
  --bundle-root ~/moex/connector \
  --profile ~/.config/moex-connector/profiles/plaza2_aggr20_md.local.yaml \
  --secret-env-file ~/.config/moex-connector/secrets/plaza2_test.env \
  --output-dir "$HOME/moex/evidence/plaza2-md/$run_id" \
  --armed-test-network \
  --armed-test-session \
  --armed-test-plaza2 \
  --armed-test-market-data \
  --max-polls 512
```

## Failure Classification

The runner classifies failure as:

- `runtime_probe_failed`
- `schema_mismatch`
- `stream_open_failed`
- `stream_not_online`
- `snapshot_incomplete`
- `zero_rows_observed`

If no rows arrive after the stream is online and snapshot-complete, likely causes include no AGGR20 entitlement,
login depth not configured for AGGR20, or no active instrument updates during the bounded window.

## Safety Model

The Phase 5D runner is read-only. It opens one replication listener and never links or calls PLAZA II publisher APIs.
It does not expose order-entry profiles, strategy behavior, public market-data ABI, or production defaults.

## Tests Added

Tests cover:

- AGGR20 profile validation and stream whitelist behavior
- all four arm flags
- rejection of ORDLOG, ORDBOOK, and DEALS
- fake `orders_aggr` projection and top bid/ask calculation
- fake runtime runner behavior
- evidence script fail-closed behavior
- profile/script offline checks

## Known Limitations

The AGGR20 projector is intentionally minimal and does not implement a general L2 book engine. It tracks only the
fields required for first live evidence. Public order-log, public order-book, anonymous trades, persistence, and C ABI
market-data export remain deferred.

## Next Recommended Phase

Next phase: `Phase 5E - PLAZA II TEST transactional order-entry bring-up`.

If AGGR20 fails due to entitlement or login-depth configuration, resolve that first if market-data visibility is
required for the operational profile. Full order-entry should still remain explicitly gated and TEST-only until the
AGGR20 evidence result is understood.
