**Scope**
Phase 4C composes the existing live TEST-only TWIME runner, the existing live TEST-only PLAZA II private-state runner, the Phase 4A reconciler, and the Phase 4B read-side attachment path into one integrated TEST bring-up surface for `plaza2_repl + twime_trade`.

This phase adds only:
- an integrated TEST runner/supervisor
- combined TEST profile and wrapper wiring
- explicit readiness and evidence reporting
- offline-safe validation tests around combined arming, readiness, reconciler feed, and read-side attachment

**Non-Goals**
Phase 4C does not add production connectivity, public market data, write-side C ABI, `plaza2_trade`, persistence, strategy logic, or a new transport stack.

It also does not replace the existing TWIME or PLAZA runners. The integrated runner composes them explicitly.

**Combined Safety Model**
Integrated external TEST bring-up is blocked unless all four arm flags are present:
- `--armed-test-network`
- `--armed-test-session`
- `--armed-test-plaza2`
- `--armed-test-reconcile`

Tracked profiles remain placeholder-only. Real hosts, runtime roots, auth files, and credentials stay in operator-local overrides, environment variables, or ignored local files.

**Credential Handling**
TWIME credentials may be loaded from environment or local files through the existing TWIME credential surface.

PLAZA II credentials may be loaded from environment or local files through the existing PLAZA credential surface.

The integrated runner prefixes child operator logs with `[TWIME]` or `[PLAZA]` and re-redacts:
- `endpoint=...`
- `credentials=...`

Evidence artifacts are therefore safe to archive without raw password or endpoint leakage.

**Startup And Readiness Sequence**
The integrated runner performs these steps in order:
1. validate combined profile shape
2. validate all four arm flags
3. create a Phase 4B ABI handle
4. start the TWIME TEST runner
5. start the PLAZA II TEST runner
6. poll both runners in one explicit orchestration loop
7. normalize TWIME live journals into reconciler inputs
8. convert the committed PLAZA private-state snapshot into reconciler inputs
9. refresh the reconciler
10. attach committed PLAZA and reconciler snapshots into the existing Phase 4B ABI handle
11. mark readiness only when both live sources and the ABI attachment are valid

Readiness is explicit and testable:
- TWIME session established
- PLAZA runtime probe succeeded
- PLAZA scheme drift validation succeeded
- PLAZA required private streams created/opened/online/snapshot-complete
- reconciler attached and updating
- Phase 4B ABI snapshot attachment valid

**Unified Health And Evidence Surface**
Native health is exposed through `Plaza2TwimeIntegratedHealthSnapshot` with:
- runner state
- TWIME validation status
- PLAZA validation status
- PLAZA runtime-probe and scheme-drift status
- reconciler health
- readiness flags and blocker
- evidence flags
- reconciled order/trade counts
- last error and last resync/invalidation reason

The CLI wrapper emits redacted operator evidence files:
- `<profile>.startup.json`
- `<profile>.readiness.json`
- `<profile>.final.json`
- `<profile>.operator.log`

The final report also queries the existing Phase 4B ABI getters so the operator can verify that the integrated runner attached committed private-state and reconciler snapshots into the shared read-side surface.

**Manual TEST Bring-Up**
Example manual operator command with placeholder-only values:

```bash
build/apps/moex_plaza2_twime_integrated_runner \
  --profile profiles/test_plaza2_twime_integrated_session_local.example.yaml \
  --output-dir build/test-output/plaza2-twime-integrated \
  --armed-test-network \
  --armed-test-session \
  --armed-test-plaza2 \
  --armed-test-reconcile \
  --twime-credentials-env MOEX_TWIME_TEST_CREDENTIALS \
  --plaza-credentials-file /secure/local/moex/plaza2_test_credentials.txt \
  --max-polls 0
```

Use `--max-polls 0` for a manual run that stays active until interrupted by the operator. The native runner still remains single-threaded at the orchestration level and stops cleanly through the existing runner `stop()` path.

**Tests Added**
The Phase 4C test set covers:
- integrated profile validation in `tools/profile_check.py`
- tracked-profile safety for combined TEST profiles
- combined arm-gating refusal in `plaza2_twime_integrated_validation_test`
- one-side-invalid refusal for missing TWIME credentials
- PLAZA scheme-drift refusal in `plaza2_twime_integrated_validation_test`
- integrated readiness, reconciler feed, log redaction, and Phase 4B ABI attachment in `plaza2_twime_integrated_runner_test`
- wrapper refusal when `--armed-test-reconcile` is omitted

All Phase 4C CI tests remain offline-safe by using the existing local TCP TWIME server and the existing fake CGate runtime fixture.

**Known Limitations**
- TEST-only; no production profile is added
- no write-side / order-entry surface is exposed here
- no persistence or journaling is added
- the integrated CLI is a bring-up/evidence tool, not a long-running service manager
- the integrated runner replays live TWIME journals into the reconciler on each explicit poll instead of introducing a new incremental cache layer

**Why This Fits 2–4 vCPU**
The integrated runner stays lean because it adds no extra transport stack, no worker pool, and no async framework. It reuses the existing single-threaded live runners and reconciler, then attaches committed read-side snapshots through the already-approved Phase 4B ABI path.

The hot path remains explicit:
- one orchestration loop
- existing TWIME and PLAZA child loops
- deterministic reconciler refresh
- explicit read-side attachment

**Next Recommended Phase**
Phase 4D should focus on operator hardening around real TEST evidence collection and bounded operational diagnostics, without broadening into production connectivity or native PLAZA II trading.
