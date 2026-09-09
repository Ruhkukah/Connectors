# Next observation-only deployment review

Status: preparation specification only; no deployment, T1 connection, order or
router interruption is authorized. The historical `96199b6` qualification must
finish its planned interval normally, including its 14:00/15:00/16:00 transitions.
Do not hot-replace it, profile it or use its VPS for performance work. Preserve all
historical evidence paths and manifests unchanged.

## Artifact and completion prerequisites

The candidate is the exact final PR #48 head, stacked on the corrected final
PR #47 head. Record both full source SHAs in a new deployment manifest. After
historical completion is confirmed, build/package that exact checkout for the
VPS's Linux architecture and runtime ABI, without modifying old checkouts, packages,
services or evidence directories. Record binary SHA-256, compiler/build flags,
CGate runtime SHA-256, scheme SHA-256 and scenario. A macOS or fake-runtime binary
is not the VPS deployment artifact. Do not substitute a synthetic CI merge SHA
for the reviewed PR head.

Do not execute deployment as part of this preparation. Pin a new package and
new empty evidence/journal directories only after the completion and authorization
checks have passed. Review the resulting artifact manifest before transfer/start.

## Authorization-window blocker

The existing qualification executable is explicitly limited to 2026-09-09,
06:58–16:10 MSK, using its original day-specific authorization token. It cannot
run a later observation scenario unchanged. Do not reuse that token or bypass
the guard. The next dated observation-only window and its gate must be reviewed
separately before the launch specification is executable. No new live window is
granted by this document or by producing the corrected source artifact.

## First scenario: observation only

- Set `MOEX_AGGR_FORENSIC_SYMBOL=ALRS-9.26` for the selected historical contract,
  isin_id 4519447, session 11702. Reconfirm that exact target/session is the intended
  contract for the newly authorized T1 window; do not silently substitute another.
- Use the protected TEST profile without an order-send arm. The existing authorized
  router configuration, credentials and AGGR entitlement are not changed.
- `MOEX_AGGR_T1_ORDER_AUTH` must be absent. Create no `order.request`; use new output
  and journal directories with no previous order epoch or request files.
- Record broker and full-client equality privately, row participant kind and
  `limits_set`; do not publish account codes unnecessarily.
- Capture only the target fut_sess_contents row, its negotiated descriptors/raw
  payload/null map and independent limit/settlement/deposit comparison. Require
  exact numeric/symbol identity and the matching AGGR instrument.
- Verify effective readiness and retained raw diagnostics. Preserve any protocol
  contradiction, including raw 179/179 values inconsistent with the current market.
- No order, order epoch, router interruption, automatic cleanup or flatten.

## Separate second scenario

Only after reviewing valid observation evidence and finding no unresolved protocol
contradiction: separately authorize controlled router stop/restart with zero own
orders, zero positions and no order epoch. Expect effective health to fall, then
Recovering, fresh POS, TRADE replay from the fresh POS anchor, fresh remaining
private/AGGR state, publisher/reply reopening and healthy observation. Preserve
failed evidence and stop on failure. No automatic command is permitted.

`POST_RECOVERY_OPERATOR_CANCEL_NOT_IMPLEMENTED`

`LIVE ORDER TEST WITH RECOVERY ENABLED = NOT AUTHORIZED`

A future explicit operator-authorized post-recovery Cancel mechanism needs its own
review. Successful observation and zero-order recovery tests do not remove this
gate. Performance and ORDLOG/L3 work remain separate follow-up PRs.
