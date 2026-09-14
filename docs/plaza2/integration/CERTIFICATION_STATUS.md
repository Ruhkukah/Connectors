# Accepted qualification status

Evidence source: PR #56 b0f80c4862be73df28df32f8f1db0e496a1b054b.
Recovery implementation: PR #55 e004251ae932a2d15736ecf3bc07d03fcf63d160.
Private sealed archive SHA-256:
279dfa464898f8af81a5d9fde54d88157e96339a4d0e05f5778f68c65977ae9f.

| Requirement | Status and scope |
|---|---|
| Account identity | PASS: exact PART client section observed live |
| Account limit row | PASS: limits_set=1 observed live |
| Zero exposure | PASS: fresh POS to TRADE/UOB evidence |
| Price formula authority | PASS: MOEX confirmed upper=settlement+limit_up; lower=settlement-limit_down |
| Graceful SIGTERM | PASS: same tested process exited 0 |
| Zero-order router recovery | PASS: two 131073 failures then fresh recovery in the same PID |
| Live order lifecycle | NOT YET TESTED |
| Working order across transport loss | NOT YET TESTED |
| Post-recovery operator cancel | NOT IMPLEMENTED in this integration base |
| Authoritative terms in Add gate | Remaining implementation gap in this integration base |

The historical September 9, September 10 and September 11 PR54 router FAILs are
preserved. New PASS evidence does not relabel them. PR51/PR56 evidence retains its
original source identity. The new integration candidate is offline-equivalent
functional code, not a newly live-qualified executable. No order is authorized.

## Price gate follow-on (offline)

Native session-price authorization and final pre-post comparison are implemented in the separate price-gate
PR; see PRICE_SEND_GATE.md. This does not promote any live order status or close the recovered-cancel gap.

## Current follow-on implementation status (offline only)

| Requirement | Current status |
|---|---|
| Authoritative session terms in Add gate | IMPLEMENTED; local/Linux price-gate suites pass |
| Explicit post-recovery operator cancel | IMPLEMENTED for an in-process persistent epoch; see RECOVERED_CANCEL.md |
| Live order lifecycle | NOT YET TESTED |
| Working order across transport loss | NOT YET TESTED |
| Restarted-process nonterminal cancellation | Not enabled; restored checkpoint remains fail-closed |

The accepted live PASS rows above retain their original source identity. The integration-base gap rows are
historical; these follow-on rows describe the new code. No new live qualification claim is made.

## Restart recovery follow-on (offline only)

Persistent v2 checkpoint restart reconciliation and explicit recovered Cancel are implemented in a separate
stacked PR; see RESTART_ORDER_RECOVERY.md. The new process stays recovery-only. Legacy nonterminal v1 remains
blocked. This supersedes the earlier implementation-gap row only for v2; every live evidence identity and
NOT YET TESTED live-order row remains unchanged.
