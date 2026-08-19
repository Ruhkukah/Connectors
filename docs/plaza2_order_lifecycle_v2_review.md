# PLAZA II order lifecycle V2 review

## Scope and verdict

This increment starts from authoritative main commit
`dc396cbe05ddae37989a12e2cef6d0ce1e8fd757` on branch
`codex/plaza2-order-lifecycle-v2`. It implements a transport-neutral order
lifecycle, runtime publisher primitives, factual private-state observations,
durable local incident journaling, and a native dry-run planner.

The offline acceptance gate passes. No production connection, TEST connection,
network command, credential, software key, account secret, broker profile, or
live order was used. The native executable contains no live transport and
deliberately refuses `--send-test-order` after validating the exact plan hash.

This verdict is not authorization for a live TEST order.

## Locked semantics used

The implementation was checked against the repository-pinned vendor material:

- `spec-lock/prod/plaza2/cgate_docs/cache/cgate_en.pdf`, sections 2.7.7 through
  2.7.9: publisher message allocation, post, and free are separate calls;
  `cg_msg_data_t.user_id` is 32-bit; `CG_PUB_NEEDREPLY` routes a reply carrying
  the originating `user_id` to a `p2mqreply` listener.
- `spec-lock/prod/plaza2/cgate_docs/cache/cgate_en.pdf`, connection processing:
  `CG_ERR_TIMEOUT` is the benign no-event result of `cg_conn_process`, not a
  generic success result for publisher operations.
- `spec-lock/prod/plaza2/cgate_docs/cache/p2gate_en.html`: AddOrder request 474
  has reply 179 containing `order_id`; DelOrder request 461 has reply 177 and
  consumes `order_id`, described as the order ID to delete.

Consequently, DelOrder is encoded only with the `order_id` from an accepted,
`user_id`-correlated AddOrder reply. The reply ID must also match a replicated
public/private identifier or alias. There is no public/private fallback guess.

## Code changed

- `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_runtime.hpp` and
  `protocols/plaza2_cgate/src/plaza2_runtime.cpp`
  - make timeout translation operation-specific;
  - expose SHA-256 for payload/plan fingerprints;
  - add publisher create/open/post/close/destroy support;
  - retain allocation, post, and free errors independently;
  - classify publisher outcomes as definitely not sent, possibly sent, or
    posted;
  - expose raw untyped reply messages with documented 32-bit `user_id`.
- `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_private_state.hpp`
  and `protocols/plaza2_cgate/src/plaza2_private_state.cpp`
  - touch only the stream that supplied an order row;
  - retain independent source commit sequences;
  - converge the two order sources by proven IDs or ext/client identity;
  - retain identifier aliases and flag conflicting identities;
  - project trade-side ext IDs for own-trade matching.
- `connectors/plaza2_trade/include/moex/plaza2_trade/plaza2_order_lifecycle.hpp`
  and `connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp`
  - add the transport and monotonic-clock abstractions;
  - add complete order observations and lifecycle control;
  - add canonical pre-send plans and exact-hash authorization;
  - enforce the first smoke policy;
  - add atomic local journals and unfinished-run identifier locks.
- `apps/plaza2_order_lifecycle_runner.cpp`
  - add a native dry-run entry point with authoritative enabled/environment
    values and no CGate open path.
- `tests/plaza2_trade/plaza2_order_lifecycle_scenarios_test.cpp`
  - add one deterministic scenario executable for the lifecycle matrix.
- `tests/plaza2_cgate/fake_cgate_runtime.cpp` and existing runtime tests
  - add publisher fault injection and an executable `user_id` reply round trip.
- CMake registers one lifecycle library, one native runner, and one new CTest.

## Safety invariants

1. Dry-run rejects all arm flags, invokes no transport, and writes only
   `pre_send_plan.json`.
2. A future send mode requires `--send-test-order` and the exact SHA-256 of the
   canonical plan generated from the same inputs.
3. Native validation rejects disabled and non-TEST profiles; these values are
   not hardcoded by a wrapper.
4. Quantity must equal one even if editable `max_quantity` is two or higher.
5. Limit type, refdata membership, tradable session, decimal validity, tick
   alignment, fresh two-sided AGGR20, passive price, independent notional and
   distance ceilings, applicable limits, and identifier uniqueness are all
   independent fail-closed checks with no override.
6. Allocation failure is definitely not sent. Any invoked, non-successful post
   is possibly sent. Successful post remains posted even if message free fails.
7. AddOrder is never retried after an ambiguous outcome.
8. Every possible/posted path reconciles to a factual safe terminal state or
   writes an unfinished orphan incident while retaining identifier locks.
9. DelOrder uses only the accepted AddOrder reply ID; it is never guessed from
   a public/private fallback.
10. Full own-trade evidence takes precedence over a cancellation-shaped order
    row. A fill cannot be labelled cancelled.
11. Source provenance and commit sequences are retained independently for
    trade replication, user-orderbook replication, current-day data, and own
    trades.
12. Plans and journals contain fingerprints, IDs, hashes, quantities, states,
    and provenance only. Broker/client/account values and raw command payloads
    are not written.

## Lifecycle transitions

| Evidence or operation | State/result | Required next action |
|---|---|---|
| Pre-send validation or message allocation fails | `definitely_not_sent` | Safe terminal; no retry needed |
| Publisher post was invoked and did not return success | `possibly_sent` | Never retry AddOrder; poll and reconcile |
| Publisher post returned success | `posted` | Observe reply and replication |
| Correlated AddOrder reply rejects and no order evidence exists | `rejected` | Safe terminal |
| Replication proves open remainder with no executions | `working` | Cancel only after accepted reply ID is proven |
| Own trades prove executions with open remainder | `partially_filled` | Preserve executions; cancel only the remainder |
| Own trades/replication prove the original quantity executed | `filled` | Safe terminal; never send DelOrder |
| DelOrder was submitted after reply-ID proof | `cancel_pending` | Poll and reconcile; do not infer success from timeout |
| Replication proves the remaining quantity removed without a full fill | `cancelled` | Safe terminal; retain any executed quantity |
| Identity conflict, missing reply ID, polling failure, or inconclusive timeout | `unresolved_orphan_incident` | Persist unfinished incident and retain ext/user locks |

## Tests and scenarios

The single `plaza2_order_lifecycle_scenarios_test` covers:

- dry-run with no arms and no transport call;
- disabled profile, non-TEST profile, conflicting modes, armed dry-run, and
  quantity two even when `max_quantity` is two;
- pre-post tick validation and message allocation failure;
- successful post plus message-free failure;
- post timeout/ambiguous submission with no AddOrder retry;
- immediate full fill;
- partial fill followed by factual remainder cancellation;
- replication timeout followed by reconciliation;
- cancel rejection and cancel timeout;
- polling failure after possible submission;
- trade-only, user-orderbook-only, and converged provenance;
- unfinished-run duplicate ext/user refusal;
- public/private identity mismatch with no guessed cancel;
- full-fill precedence over a cancellation-shaped row;
- independent projector stream commits and source convergence;
- operation-specific timeout translation and a known SHA-256 vector.

The runtime adapter test additionally covers publisher allocation/post/free
classification and proves that publisher `user_id` 703 is returned by an
untyped reply listener as AddOrder reply message 179 with a raw payload.

## Baseline versus final metrics

Both measurements are local Debug/Ninja builds with AppleClang 17 and the same
default Ninja parallelism. Wall-clock numbers are environmental observations,
not performance acceptance thresholds.

| Metric | Baseline `dc396cbe` | Final branch |
|---|---:|---:|
| CMake targets | 222 | 226 |
| CTests | 153 | 154 |
| Configure time | 1.93 s | 1.72 s |
| Build time | 9.52 s | 9.10 s |
| Full CTest time | 32.65 s | 31.60 s |
| Full CTest result | 153/153 pass | 154/154 pass |

ASan plus UBSan passed the lifecycle scenario, runtime adapter, runtime probe,
and offline no-send guard (4/4). Apple ASan does not support leak detection, so
the successful run used `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.

The native CLI dry-run also produced
`pre_send_plan_sha256=abc22ebd9c33f95198ff2c460633f0a7015e90e3af592ff1f2376e7a1acc9615`
for the documented offline fixture. Inspection confirmed that the plan contains
the payload hash and smoke verdicts but no broker or client value.

## Selective PR #24 reuse

PR #24 and commit `24e4619e29244a6c11f368044550aabf04653a86` were inspected
with `git show`/`git diff` only. No commit was cherry-picked and no file was
copied wholesale.

The useful publisher symbol-loading and handle-lifecycle ideas from these two
historical files were selectively reworked into the current runtime boundary:

- `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_runtime.hpp`
- `protocols/plaza2_cgate/src/plaza2_runtime.cpp`

The new implementation changes the unsafe historical semantics: timeout is not
generic success, allocation/post/free results are separate, successful post is
not erased by free failure, replies use the documented raw listener contract,
and lifecycle decisions are made by the new controller.

The following historical scaffolding was deliberately not reused:

- `apps/moex_plaza2_trade_test_order_runner.py`;
- `apps/plaza2_trade_test_order_entry_runner.cpp`;
- `connectors/plaza2_trade/include/moex/plaza2_trade/plaza2_trade_test_order_runner.hpp`;
- `connectors/plaza2_trade/src/plaza2_trade_test_order_runner.cpp`;
- `docs/plaza2_phase5e_test_order_entry_bringup.md`;
- both `profiles/test_plaza2_trade_order_entry*` files;
- `scripts/vps/plaza2_trade_test_order_evidence.sh` and the package-script
  expansion;
- the phase-specific runner/script tests;
- the historical live-session changes and fallback order-ID logic.

## Remaining blockers before a live TEST order

1. Implement and review a concrete TEST-only transport that composes the
   publisher, reply decoder, trade/user-orderbook replication, and reconciliation
   APIs. The current native runner intentionally has no such transport.
2. Bind smoke evidence to authoritative committed refdata, session, AGGR20, and
   limits snapshots rather than operator-supplied offline fixture flags.
3. Validate actual installed TEST CGate runtime/scheme compatibility and the
   reply payload decoder against the locked broker TEST environment without
   placing credentials or endpoint secrets in Git.
4. Define restart-time orphan reconciliation and operational journal ownership,
   permissions, retention, and alerting. Unfinished IDs currently remain locked
   by design.
5. Review a generated plan, provide its exact hash through a separate explicit
   authorization gate, and conduct a separately approved one-order TEST run.

Stop here. No VPS run, live TEST submission, production work, C ABI rewrite, or
new protocol phase is part of this increment.
