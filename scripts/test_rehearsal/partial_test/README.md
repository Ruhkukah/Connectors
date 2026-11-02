# Isolated partial-fill TEST harness (unarmed)

This review package implements the optional aggressive partial-fill/Move
scenario with quantities 2–10. It does not modify ordinary q2 rehearsal code,
the running sealed job, production code, deployment files or timers. Importing
performs no IO. `python -m scripts.test_rehearsal.partial_test` always refuses;
there is no configuration or argument that arms it. Nothing in this package
starts/stops a product, supervisor, guardian, router or session.

## Scope and native adapter contract

One frozen `FrozenScope` selects a single current REFDATA-resolved instrument
(`ALRS-12.26` or `CNY-12.26`) and the exact same-day date, candidate source,
binary, green CI run, private login, allocation range and instance. The future
protected deployment must capture the fixed notional budget from the fresh
best ask when sealing: at most selected maximum quantity × that ask. The budget
never rises. The scoped process has quantity/position ≤10, one live order,
rate 5/s, at most three natural attempts, and a 600-second observation budget.
There is no whole-session or other-instrument risk exception.

`scope.launcher_arguments()` returns only a reviewable **risk argument fragment**
for the actual CLI parser, not a complete CGate invocation. The protected
deployment supplies pinned executable/socket/environment/schema/config paths,
preserves credentials and durable IDs, verifies binary and exact CI, and owns
the existing guardian. Actual accepted flags are defined in
`apps/moexctl.cpp` and `connectors/connector_host/src/operator_config.cpp`.
Login uses `--login-env MOEX_LOGIN`; the login value is never placed in argv.

Future integration injects `GuardedCli(scope, invoke, snapshot, proof, ...)`:

- `invoke(('plaza2', 'cmd', exact_command))` must call **actual moexctl CLI**
  through the protected socket, with an argv list and no shell/SDK. A decoded
  `{"ok": true}` is only command-channel acceptance; native reply/commit and
  reconciled status establish exchange outcomes. Timeout/nonzero/refusal or
  malformed output stops new exposure; no uncertain Add is retried.
- `snapshot()` returns a freshly collected unmodified product `status` object.
  Existing `configuration.risk`, transport/listener reconstruction, replay/POS
  LifeNum, current session terms, book and native pending-fill proof fields are
  required. A POS zero with a pending fill reservation is not flat.
- `proof()` returns a fresh `RuntimeProof` from the protected adapter's exact
  binary/process identity, TEST-only ownership, same allocation, minimum 7000
  remaining IDs, reservation coverage and negative control, login/namespace,
  and exact review-head green CI checks. Native source/binary pins remain
  separate from `review_ci_head`; if the latter differs, the protected adapter
  must prove the intervening diff is scripts-only (`scripts_only_comparison`).
  These are external verified facts, not values
  derived by copying the desired configuration. Defaults fail closed.
- Launcher, effective native limits, candidate selector, exact commands,
  sequence snapshots and `guardian_check` all use that same immutable scope.
  An ordinary q2 process cannot be upgraded through this controller.

`PartialHarness` also receives a journal offset reader, an observer factory
backed by `GeneralCommittedDealObserver`, a private record callback and an
independent reconciliation callback. The latter returns scope digest, fresh
post-command observation time, same-instance/complete flags, zero working /
queued / nonzero-position counts, independently collected `native_state` and
`runtime_proof`. It must be collected after the last command, in the same TRADE
LifeNum, and pass all complete reconstruction and zero pending-fill checks.
Reusing a pre-Add flat snapshot cannot finalize an attempt. This is an account
reconciliation receipt, **not** terminal shutdown proof. `verify_terminal`
separately requires terminal=true, cancellation and flatness exactly true,
exit 0, zero clients, no inventory/shutdown errors and no guardian failure.

## Executed scenario logic

1. Prove current native readiness, complete flatness, reservation/ownership/IDs
   and the exact scoped native configuration. Wait for fresh best-ask size 1–9.
2. Select best-ask size + 1 contracts, bounded by 10, at that ask. Require the
   fixed budget and per-request quantity × fresh ask bound. Recheck the quote
   immediately before one exact Day Buy. There is no price escalation or
   command retry to force a fill.
3. Observe complete committed `user_deal` transactions and exactly one credited
   execution per native deal identity. Only 0 < executed < original quantity,
   with matching fresh owned order/status and remaining quantity, is partial.
   Zero/full fills are `NOT_OBSERVED` after correct accounting and cleanup.
4. Recheck after rate pacing and again at the final command boundary. An extra
   fill in either interval aborts the Move attempt. Preserve original total
   order quantity for regime 3, including already confirmed fills.
5. Move a buy to fresh bid − one tick, or the bid itself if the tick would cross
   the lower bound. Require grid/bounds, price ≤ bid and < ask, and fixed
   notional. The old ask − tick rule could remain above the bid in a wide
   spread; this implementation fixes that rejection without raising limits.
6. Require the matching native regime-3 Move, code-0 reply, later TRADE commit,
   changed order alias and consistent status. Bind the new alias only after
   that proof, then include its subsequent deals in exact-once accounting.
7. Cancel remaining owned quantity. Flatten only the current complete signed
   position with uniquely identified risk-reducing IOC requests, bounded to three
   proven terminal attempts, never through zero. Check native deal/credit
   accounting, POS catch-up and independent fresh flatness before another Add.
   Unknown cancellation/identity/accounting refuses new attempts. Cleanup
   failures are recorded separately from the initiating failure.

If a fresh crossing price rises above the captured ask, cleanup reduces each
IOC quantity to `min(abs(position), quantity_limit, fixed_budget // price)`.
Thus q10 may flatten in q9+q1 without raising the frozen notional cap. If the
bounded attempts cannot prove flatness, cleanup fails explicitly.

Results retain Add/Move/end journal offsets. The copied observer and cursor
record their source hashes in `evidence.py`; no `/tmp` module is imported.
The ordinary rehearsal observer and its q2 bound remain unchanged.

## Validation and activation boundary

Run the repository safeguard suite with:

```sh
python3 -m unittest discover -s scripts/test_rehearsal -p 'test_*.py'
```

Regressions use the actual controller, typed command guard, journal cursor and
committed-deal observer with an injected deterministic exchange fixture. They
prove software behavior; they are not native TEST qualification evidence.
No scoped deployment, native session, natural partial fill or certification
PASS is implied. A future activation requires separately reviewed protected
adapter integration and fresh deployment/flatness gates; this task arms none.
