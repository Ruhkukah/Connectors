# MOEX price-formula correction — 10 September 2026

Status: **MOEX_PRICE_BOUND_FORMULA_CONFIRMED**.

The user supplied this explicit correction after the retained router report was prepared: MOEX Support confirmed its previous lower formula contained a typo. The confirmed formulas are:

```
upper = settlement_price + limit_up
lower = settlement_price - limit_down
```

This supersedes the unresolved-price status in REPORT.md and the original summary.json, which are preserved as historical records. The original plus-lower wording remains in the protected user-supplied support context; the original exchange email itself has not been provided. This addendum records the user-relayed correction without claiming direct access to that email.

Historical regression: 2077 / 184 / 184 gives lower 1893 and upper 2261. The recorded market 2042–2045 lies inside. Raw fields remain unchanged. No absolute value, alternate reference or inferred sign is allowed.

The follow-up draft was never sent and is now superseded; no further lower-sign question is needed. Live orders remain unauthorized. The router FAIL, exact source/binary/configuration/evidence identities, restored router, account correction and previous shutdown PASS are unchanged. POST_RECOVERY_OPERATOR_CANCEL_NOT_IMPLEMENTED remains unchanged.

Offline implementation is continuing in codex/plaza2-session-terms from ca48ae94529f27926c3dffcef9ed68d96a5afaa9. This does not change the tested live binary.

## Implementation contract

`FutureSessionTerms` retains exact scale-five settlement, raw upper/lower distances and minimum step from one `fut_sess_contents` row, with replication ID/revision and source LifeNum. Bounds are computed once when that row is staged and become visible only with its transaction commit. A current-session instrument-map lookup checks the requested session and LifeNum against the committed source revision. This deliberately uses the existing current-session representation; it is not an archive of every instrument/session pair.

The parser rejects missing/null, malformed, over-precision and overflowing decimals. Arithmetic checks addition and subtraction directly, including INT64_MIN, without negating the subtrahend. Derived bounds remain separate from raw values. `interval_valid` means complete, non-overflowing ordered arithmetic with a positive step; it is not trading permission or proof of live transport health. Effective host readiness, account/replay checks and authorization remain independent and unchanged. No live Add price gate is promoted by this PR.

The coherent row survives unrelated instrument updates without mixing `fut_instruments` or `settlement_price_open` values into its reference. Deletion, ClearDeleted and LifeNum replacement use existing source invalidation. No new global reference-data scan or separate replication engine is added. Existing full-map staging and snapshot costs remain for the separate performance tranche.

Validation covers the historical numeric regression, signed arithmetic, missing values, exact precision/range boundaries, overflow/underflow, uncommitted visibility, source/table separation, deletion, ClearDeleted and LifeNum/session mismatch. Synthetic mixed-case account tests inspect independently laid-out AddOrder bytes and distinct canonical plan hashes, without a publisher post. Existing ambiguous/missing PART and effective-health tests remain in the suite.

The failed router scenario is independent: runtime returned CG_ERR_INTERNAL on MQ:SOCK_CLOSED before rebootstrap. No retry classification or live runtime change belongs to this PR. The authoritative model is prepared for later consumers; the qualification-only historical price collector is not silently reinterpreted here.

Local validation: full macOS Release CTest **179/179**; ASan+UBSan relevant component tests **126/126**; repository/source style, Unicode and diff guards passed. Linux Release and ASan+UBSan+LeakSanitizer are required in CI; macOS sanitizer results do not establish Linux leak checking. No timing results from these tests are performance evidence.
