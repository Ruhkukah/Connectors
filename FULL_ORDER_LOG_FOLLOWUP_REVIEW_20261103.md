# PR70 follow-up to9568782b — 3 November 2026

This work is on the isolated PR70 branch only. PR68, both certified AGGR artifacts and their state/configuration remain unchanged. Nothing native is started and no timer is installed. Release/CI receipts identify the final commit and separate stopped release; they do not certify FOL or prove T0 order acceptance.

## 1. Stream liveness and clock tolerance

The owner-thread FOL forwarding handler refreshes activity only after a successfully handled StreamData or transaction commit, including filtered other-ISIN and sys_events rows. OPEN/CLOSE/error/stop reset activity; empty connection-process turns and private TRADE traffic cannot revive it. Add/Move require ACTIVE FOL with activity<=30s, plus the instrument's committed validity/session/epoch/revision/positive two-sided uncrossed book. Quiet instrument timestamps no longer expire a valid book while its stream is active. Exchange time must be present and no more than1s ahead of host UTC, with an inclusive boundary and overflow-safe comparison. The same proof runs at admission and final native post.

Core and actual-executable regressions distinguish an old quiet book from a dead FOL stream, test30s/+1ns and1s/+1ns boundaries, and keep cancellation available. Native stream activity is independent of metrics collection and logging verbosity.

## 2. Session authority follows retained orders

Each retained order carries its own session. Bounded per-instrument session counts derive authority at commit without scanning the full book. A mixed/unknown book refuses entry; after old/unknown identities are removed, surviving new-session orders become authoritative without a listener reset. Cancel, full trade, replAct and finite ClearDeleted all decrement the counts. An inconsistent update cannot retag an old resting order; that identity is unknown until removed. Tests exercise old-session deletion followed by new rows and mixed/empty/recovered books.

## 3. FOL-only cleanup

`plaza2 run --cancel-only` uses the same FOL executable and ORDLOG account, original journal/checkpoint/instance/range and existing private reconciliation. It opens no FOL/AGGR/DEALS/DTC. Immutable session and host gates refuse Add/Move even after `kill off`; cancellation remains available. Invalid recovery profiles are rejected before journal/ID reservation. The actual executable regression starts against a deliberately incompatible FOL scheme, reconstructs a known order and cancels it with only private listeners.

`FULL_ORDER_LOG_CLEANUP_20261104.md` specifies the process, login, command, original-state preservation, private-state proof, deadline and escalation when recovery is unavailable. Resting tests use Day only. An actual position is not silently flattened by this mode or called a clean result.

## 4. Binding, allocation and clock evidence

The owner's pointer to `MOEX_Claude_handoff_CURRENT.txt` was re-read, followed by an independent decode of restricted20October native `FORTS_USER_REPL`, `FORTS_REFDATA_REPL` and `FORTS_PART_REPL` captures for ORDLOG and AGGR. The current private record is `/home/azgaldov/moex/full-order-log-4102/deployment/20261104/account-binding-private.json`; broker/client/login environment values are in its0600 `binding.env`. No AGGR checkpoint, journal or namespace was copied.

Exactly one matching USER and REFDATA user row and a matching PART establish the seven-character account; client level3, `sma_flags=0`, `sma_status=0`. ORDLOG operation mask68 and AGGR4 are recorded, not treated as complete trading-right proof. Both logins map to the same account. No fresh native readiness/flatness is inferred from the historical snapshots. The first passive Day Add in block B proves order-entry acceptance; any native rejection stops B and is reported without retry.

AGGR and FOL trading never overlap; instrument-scoped DelUserOrders with ext_id0 affects both logins on this account. Risk limits include account-wide positions, working orders and external activity. A dedicated FOL ID registration must exclude AGGR2000070000–2000119999 and all other historical ranges, under the canonical allocation lock. Its separate receipt is preparation only; no startup IDs are consumed tonight.

The runbook specifies fresh `chronyc tracking` System-time residual conversion (fast positive, slow negative), selected source, UTC receipt time, uncertainty and NTP synchronization. `--clock-offset-us` records the measurement; it is not an assumed zero or a clock-setting command. The3November17:11MSK historical residual was+0.637us (+1us rounded), not tomorrow's measurement.

## 5. Accepted mass-cancel reconciliation

An empty/heartbeat/unrelated TRADE commit no longer releases an accepted186 scope. Only exact account/instrument/session/order identities with validated post-barrier local TRADE observations can enable per-ID cancellation fallbacks. USERORDERBOOK alone, another instrument/account/session, pre-barrier rows or partially observed survivors cannot supply the proof. A new/relisted identity cannot inherit a previously enabled group phase; enqueue and both retry paths check its exact target witness.

Even a freshly observed working row is not proof that the exchange's bulk operation completed. It enables risk-reducing DelOrder only; the bulk Add gate stays closed until every scoped order is terminal. No server replRev is compared with an unrelated local counter or reply revision. Business-rejection fallback behavior is retained. The previous empty-commit test expectations are corrected and negative controls restoring either old unsafe path make the new regressions fail.

`order_manager.hpp/.cpp` changes affect an ordinary trading binary rebuilt from PR70 as well; they are **not** behavior-neutral and are intentionally absent from PR68. Existing shared-source reviews remain applicable. No claim of binary/ABI/performance equivalence is made.

## Prepared day

`FULL_ORDER_LOG_T0_RUNBOOK_20261104.md` covers A07:15 read-only acceptance, B08:00 lifecycle/conditional IOC, C10:20–11:20 no-order observation and D14:50–15:30 settlement with one passive Day Add/Del. Every block needs a new owner message and bounded TERM deadline; stop FOL and then ORDLOG router by15:45MSK. Default vendor logging, no PROD, no scheduled starts. Intentional fills remain separately authorized.
