# PLAZA 9.9 C2 preflight contract

2026-09-07. **Production C2: BLOCKED. No identity key is frozen.** This freezes established semantics, conservative rejection
policies and an executable conditional reference model. It does not turn missing protocol evidence into a protocol contract.
No T1, publisher calls, exchange orders, production L3 code, C ABI or managed API changes. PRs #38–42 remain unmerged.

## Authority and evidence

The [Phase B contract](P2ORDBOOK_ORDLOG_RECOVERY_9_9.md) and
[source lock](../../spec-lock/test/plaza2/public99/wire.json) remain authoritative for the settled ten tables / 135 fields.
Sources used here are the same locked documents and SDK, not a new scheme qualification:

- [CGate manual](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/cgate_en.pdf), 31.07.2026:
  receiving data; data schemes; p2ordbook parameters; `cg_lsn_getscheme`, pp. 11–16, 32–38.
- [SPECTRA manual](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/p2gate_en.html): Orders — general information;
  Change of order ID during iceberg orders operations; Trading day change; Service replication fields; Recovery in case of
  Exchange infrastructure failure; ORDLOG tables 14–15; ORDBOOK tables 38–43.
- SDK 9.9.1853 `samples/src/samples/c/orderbook/ordbook.cpp`: dynamic scheme lookup at OPEN; direct remainder for add/execute;
  terminal cancel; LifeNum/CLOSE cleanup. Its instrument-filtered example is not a global identity or cross-pair proof.
- [Offline SDK probe](../../tests/fixtures/plaza2_c2/sdk_probe.cpp),
  [output](../review/plaza2_c2_preflight_20260907/sdk_probe.log), and
  [test/model sources](../../tests/plaza2_cgate/plaza2_c2_preflight_test.cpp).

`PASS_OFFLINE` always names its scope. A model pass does not establish a missing exchange rule. `REQUIRES_T1_CAPTURE` identifies
observable negotiation/chronology gaps. `BLOCKED` includes questions requiring written MOEX authority; a finite capture cannot
prove universal ID uniqueness or all future reuse cases.

## Composite negotiation

Regular URL: `p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL`.
Defaults: `snapshot.data=orders`, `online.data=orders_log`, `snapshot.bind=info.trades_rev`.
Explicit offline client schemes use `online.scheme=|FILE|.../ordLog_trades.ini|CustReplScheme` and
`snapshot.scheme=|FILE|.../ordbook.ini|CustReplScheme`.

For the multileg candidate, add `snapshot.data=multileg_orders;online.data=multileg_orders_log`; retain `info.trades_rev`.
The parameter contract selects one named data table per side. It does not document a comma-separated pair list or atomicity
between logical pairs. Two independently configured pairs are the test hypothesis. Neither a combined regular/multileg
listener nor the actual multileg handoff/coordination is qualified offline.

The official Linux amd64 SDK ran in Docker with `--network none`. The only connection-open attempt targeted container
loopback port 1. It returned 131073; vendor logs identify `MQ:SOCK_CONNECT`, connection refused. Environment creation and
both listener constructions returned 0. For both pairs, pre-OPEN `cg_lsn_getscheme` and `cg_lsn_open` returned 131077
(`CG_ERR_INCORRECTSTATE`); scheme pointer was null, listener state CLOSED, callback count zero. This limited experiment
cannot prove that every conceivable offline simulator is impossible. It provides no negotiated fixture. The documented
availability point is OPEN, which requires a working connection in this setup. No Access Server simulator was supplied.

| Mapping | Actual negotiated evidence | Synthetic test fixture, deliberately NOT vendor indices |
| --- | --- | --- |
| Regular | REQUIRES_T1_CAPTURE | 0 orders_log, 1 info, 2 orders; reversed: 0 orders, 1 info, 2 orders_log |
| Multileg | REQUIRES_T1_CAPTURE | 0 multileg_orders_log, 1 info, 2 multileg_orders; reversed analogously |
| Control delivery | REQUIRES_T1_CAPTURE for exact composite chronology/exposure | OPEN, TN_BEGIN, rows, TN_COMMIT, ONLINE, LifeNum, ClearDeleted, REPLSTATE, CLOSE |

The test consumer binds **each OPEN** by table name and validates full message size, field names/types/offsets/sizes, field count
and descriptor chain. It uses the resulting composite ordinal, never source ordinal. Regular snapshot and log are both
128 bytes; size alone cannot select the decoder. Wrong index, row-kind confusion, malformed layout, null values and conflicting
same-revision field bytes fail closed. The fixture accepts exactly its three model tables; actual extra service tables and
whether `info` itself is exposed must be captured before a production binder is written.

This extends the existing dynamically loaded fake CGate library. Its existing ABI declarations are shared in a test header;
no connector API changes. The new consumer calls the fake native callback ABI directly, deliberately leaving the settled
production raw ORDLOG adapter unchanged. Descriptor permutation proves binding logic, not vendor negotiation. There is no
invented `CG_MSG_ERROR`: tests inject listener ERROR state and supervise it. No publisher function is invoked.

## Identity: unresolved domain, proven examples

| Question | Established evidence / disposition |
| --- | --- |
| Global / LifeNum / session / instrument uniqueness | Public field says order ID, or visible iceberg part ID. No universal uniqueness scope found. **BLOCKED**, no key selected. |
| LifeNum reuse | Old generation must be discarded. Whether numeric public IDs may repeat is unspecified. **BLOCKED** for identity, invalidation PASS_OFFLINE. |
| Clearing / trading-day rollover | Ordinary multi-day relisting deletes old order and adds a new ID. Iceberg clearing example assigns new private/public IDs. These do not prove universal non-reuse. |
| ClearDeleted reuse | Refers to replication rows/revisions, not allocation of public IDs. No identity-reuse rule may be inferred. **BLOCKED**. |
| Instrument/session change | No proved legitimate in-place same-ID change. Reject contradiction; universal immutability still **BLOCKED**. |
| Multileg domain | Field has the same definition; no proof it shares a global ID namespace with regular orders. **BLOCKED**. |
| Snapshot/log comparability | Both identify the same public order/visible part in the documented snapshot-plus-log reconstruction. PASS_OFFLINE for that logical relationship. |
| Pre-bound orders | A surviving active order is supplied by snapshot; subsequent execution/cancel updates that ID. Covered by explicit callback and boundary tests. |
| Price/side/original quantity change | General MoveOrder deletes and adds with a new code; iceberg refresh gets a new public ID. Do not interpret changing operation amount as changing original size. |

The model's generator assigns unique `(logical pair, synthetic ID)` values and keeps session/instrument/side/price fixed within
one active identity. This is a **test-domain assumption**, not a conservative production-key choice. A generation owns all model
state. Same numeric ID in a new generation is tested by fresh bootstrap; that tests isolation, not exchange allocation policy.

`public_amount` is the quantity **in the operation**, not a stable original size. Snapshot provides `public_init_amount`;
ORDLOG has no corresponding original-size field. Insertion equality between operation amount and remaining amount is not an
explicit invariant in the reviewed field descriptions. The model does not impose it or invent an original-amount field.
Written MOEX confirmation is needed for the smallest correct identity domain and exhaustive same-ID invariants.

## Mutation table and conservative classifications

| Source / action | Preconditions within the conditional model | Mutation | Terminal |
| --- | --- | --- | --- |
| Snapshot row | Valid committed publication, positive direct remainder, no duplicate active identity | Seed active state using `public_amount_rest`; snapshot `public_action` is not replayed | No |
| ORDLOG 1 add | New active identity, positive remainder | Insert direct `public_amount_rest`, retain exact price/status | No |
| ORDLOG 2 execute | Known active identity, same session/instrument/side/price, nonnegative nonincreasing remainder | Replace remainder directly; never subtract `public_amount` | Yes if remainder 0 |
| ORDLOG 0 cancel | Known active identity, same immutable attributes | Remove identity; do not derive active state from cancellation quantity | Yes |

Actions 0/1/2 and the direct remainder field are official semantics. The SDK sample uses direct remainder on add/execute and
removes on cancel. Cancel field descriptions remain generic: operation quantity and remaining quantity. No exact cancel
`public_amount_rest == 0`, `== old rest`, or canceled-quantity equality is frozen. The model permits a nonzero cancel remainder
and removes the order. Normal price uses `price`; multileg `price` and `rate_price` are documented unused, so model multileg price
uses `swap_price`. `xstatus` and `xstatus2` are retained; the model does not implement every synthetic/auction/negotiated flag rule.

| Event | Classification / response |
| --- | --- |
| Exact last known revision and all qualified field bytes equal | Idempotent duplicate, no second mutation |
| Earlier known revision and all fields equal | Legal replay, no mutation |
| Unknown revision at/below established frontier | NeedsResync; do not silently skip possibly conflicting rows |
| Same revision with changed field, including non-book field | NeedsResync |
| Add already active, absent execute/cancel | NeedsResync unless proven identical known replay |
| Increasing execute remainder, session/instrument/side/price change | NeedsResync; no silent repair |
| Negative remainder/operation amount, invalid direction, unknown action, invalid schema/index/null | Fatal schema/protocol corruption in model; invalidate and stop |
| Changing operation amount | Valid field variation; it is not original amount |
| `replAct != 0` on relevant table | NeedsResync in this preflight; replication-row deletion is not automatically an exchange cancellation |

The callback reference hashes every qualified field excluding padding for replay comparisons. The pure semantic model compares
all its event fields. This bounded test digest is not a production collision-proof dedup design. C1's raw field-aware replay
handling remains unchanged. Revision jumps are not proven loss. Snapshots carry no full historical dedup ledger: unrecognized
pre-bound delivery is a contradiction in this model, not a license to discard it. Actual CGate replay patterns require capture.

## Snapshot validity and handoff

Official publication states: **0 = in progress, 1 = done**. A snapshot may span publication work and be inconsistent before 1.
`info.trades_rev` is the last processed revision at snapshot creation; `trades_lifenum` identifies the corresponding log life.
`orders_currentday`, `multileg_orders_currentday`, `info_currentday` are alternate start-of-day datasets, not extra mandatory
parts of the default current snapshot. No current-day table is admitted by the model default-pair binder.

A candidate snapshot becomes usable only after a complete transaction commits matching-life `info` with publication 1 and a
nonnegative bound, and its order records are valid. A committed publication alone is not Ready: ONLINE and mandatory drain
are also required. The model accepts one info identity updating 0→1 across transactions and either info-before-orders or
orders-before-info. Changed info identity/bound/life, 1→0, missing info, invalid publication or incomplete transaction reject.
These are conservative **model policies**, not proof of composite callback chronology. Exact order/info exposure, multiple
publications during bootstrap, and whether all mandatory transactions are surfaced remain **REQUIRES_T1_CAPTURE**. If `info`
is consumed internally, define a documented substitute witness before production; never manufacture a usable-publication flag.

CGate owns the snapshot/log race and bound continuation. No separate application race buffer is introduced. No undocumented
second online-complete event is assumed. Queue completion is an application condition after the documented ONLINE event.

## Recovery state contract

Allowed normal path: Disabled → Opening → Snapshotting → CatchingUp → Ready. Opening requires a fresh listener; OPEN binds its
schema. A first valid LifeNum supplies the generation's life. Snapshot commits accumulate mandatory work; ONLINE with a usable
snapshot enters CatchingUp. Draining all committed mandatory work enters Ready. Starting a new transaction leaves Ready until
commit and drain. Unsupported ordering fails closed, including data/ONLINE without bootstrap or nested/missing transactions.

Ready means: current generation and life; valid complete snapshot; ONLINE observed; no incomplete mandatory transaction;
no loss, callback/queue failure or unresolved contradiction; drained frontier equals committed frontier. A borrowed handle
also must match the local generation. Model book/index/revision ledger/frontier/pending work are private until Ready. There is
no materialized depth cache in the reference model; all future caches/views must use this same generation invalidation rule.

| Trigger | Immediate disposition | Recovery |
| --- | --- | --- |
| LifeNum change | Invalidate generation, all data/handles/readiness/frontiers, pending transaction and drain counters | Snapshotting; during Recovering remain Recovering until fresh OPEN |
| CLOSE | Stale, clear generation; before valid snapshot or before ONLINE never Ready | Recovering → fresh Opening |
| ERROR, queue loss, callback contradiction | NeedsResync, clear generation | Recovering → fresh Opening |
| Fatal schema/action corruption | Failed, no healthy stale book | Operator/source correction; no blind retry loop |
| Simulated retention-equivalent open failure | No OPEN/scheme/Ready; invalidate | Retry at 1000 ms, maximum three attempts per model instance; then Failed |
| Process restart | No persisted composite L3 checkpoint exists | Fresh bootstrap; raw REPLSTATE is not sufficient |

The one-second interval and three-attempt budget are explicit model policy, not an exchange retention guarantee. No numeric
retention window or arbitrary-downtime recovery is claimed. Exact vendor retention errors and restoration behavior need T1.
LifeNum tests cover snapshot, after snapshot/before ONLINE, after ONLINE, inside log transaction, mandatory backlog and recovery.
CLOSE/error tests cover before usable snapshot, before ONLINE and Ready. Failed generations cannot acquire health from a later
token. `cg_lsn_open` settings are empty for composite; no raw C1 resume token is passed.

## ClearDeleted per table

| Table | Replication meaning | Active-state model policy |
| --- | --- | --- |
| orders | Snapshot rows older than boundary removed; MAX means table retransmission | Ambiguous relevant snapshot invalidates → NeedsResync → fresh bootstrap |
| multileg_orders | Same operation on that snapshot table | Same conservative policy, independent logical pair |
| orders_log | Historical operation rows compacted; not an order cancel | Proven historical-only compaction would not mutate active orders; otherwise NeedsResync |
| multileg_orders_log | Same operation on that log table | Same independent policy |

Strict comparison is `replRev < boundary`. MAX can reset source revision progression; do not infer public-ID reuse.
An unrelated, validated service-table notification need not clear active books. The pure model demonstrates that non-relevant
or explicitly certified historical-only markers leave state untouched. **The callback consumer never assumes that certificate:**
all its relevant markers request resync. Tests deliver markers at both regular and multileg snapshot/log indices. A new
production policy may narrow these rebuilds only with authority; selective cancellation from compaction is forbidden.

## Equivalence and bounds

The independent generator builds exchange-like truth without calling the mutation model. Full ORDLOG reconstruction is compared
with that truth and with snapshots at start/end/intermediate and seeded arbitrary boundaries. Regular, multileg and mixed
histories include add, partial/full execution, cancel, surviving pre-bound orders and deep books (10,000 inserted identities).
The mixed oracle is a union of independently scoped synthetic pairs, **not a claim of cross-pair transaction atomicity**.

The tests compare complete ordered maps and deterministic FNV-1a hashes of pair, public ID, session, instrument, side, exact
integer price, remainder, status and status2, plus exchange generation. Local handle epoch changes on restart and is separately
tested invalid; it is not included as though it were an exchange-generation difference. Snapshot frontiers are tracked per pair
in the oracle. This does not resolve how a real multileg composite exposes its bound.

The bounded run uses 32 seeds × 3 pair modes plus 3 large histories: 99 histories, 1,136 snapshot equalities, 1,136 interrupted
restart equalities, 396 one-element illegal histories at seeded offsets. Explicit anomaly/recovery/callback cases run first.
The fake callbacks exercise four pair/permutation combinations, 18 fault combinations, and two open-error cases. Missing commit,
callback error, LifeNum, ClearDeleted, malformed index, replay and non-book field conflicts are covered. Mandatory staging is
bounded at 20,000 events and overflow invalidates. The test map/replay ledger deliberately prioritize transparent correctness;
they are not a production allocator or throughput claim. Real exchange legal-history completeness is still BLOCKED by identity
and the unqualified callback contract.

## Production data structures: design only

After identity is proven, use one owner and a pre-sized native hash index from that key to a stable slot. Store integer prices,
quantities and numeric identifiers. A bounded slot pool with a free list makes erase by ID predictable. Use per-instrument active
slot lists and ordered price-level indices holding exact aggregate quantity and order count. Store each order's level/slot
handles for direct removal. No per-event strings, float prices, global per-record lock or whole-book copy.

Build snapshots in the inactive generation's bounded storage; publish only a complete generation. Maintain small transaction
staging and validate before commit. Borrowed views carry generation plus slot/version and become invalid immediately on loss.
Generation switching invalidates access in O(1); storage reclamation occurs separately under the single owner, with a capacity
bound for old and new generations. Canonical sorting/hashing is for tests/diagnostics, not every update. Exhausted capacity must
invalidate health explicitly. Deep-book capacity sizing and full L3 throughput remain future C2 work; D0 raw rates are unchanged.

## Authorization gates

| Required gate | Status at closeout | Exact remaining evidence |
| --- | --- | --- |
| 1 Regular negotiated table/index mapping | REQUIRES_T1_CAPTURE | OPEN descriptors, all actual tables, callback indices |
| 2 Snapshot validity | REQUIRES_T1_CAPTURE | Publication states are PASS_OFFLINE; actual composite info/transaction chronology is missing |
| 3 Snapshot→online boundary | REQUIRES_T1_CAPTURE | Documented bind is PASS_OFFLINE; actual composite boundary callbacks/coverage missing |
| 4 Order identity | BLOCKED | Written domain/reuse/same-ID invariants; capture examples cannot prove universal scope |
| 5 Normal action/remainder semantics | PASS_OFFLINE | 0/1/2, direct rest, terminal cancel; no claim of insertion equality or exact cancel-quantity relation |
| 6 LifeNum invalidation | PASS_OFFLINE | Conservative model generation disposal at all six positions |
| 7 Restart/failure semantics | PASS_OFFLINE | Conservative fresh-bootstrap policy, bounded retry, no token-only L3 recovery |
| 8 Continuous/snapshot equivalence | BLOCKED | Conditional model PASS_OFFLINE; real protocol equivalence awaits gates 1–4 |
| Multileg negotiated mapping/coordination | REQUIRES_T1_CAPTURE | Exact pair setup, descriptors, callbacks, bounds; no scope downgrade |
| ClearDeleted selective active-state semantics | REQUIRES_T1_CAPTURE | Conservative rebuild model PASS_OFFLINE; no selective cancellation approved |
| Complete production projector / API / performance | BLOCKED | Explicitly outside this authorization |

**C2 production implementation is not authorized.** The eight required gates are not all PASS_OFFLINE. Continue only with
[the market-data capture scenario](C2_MARKET_DATA_CAPTURE_9_9.md) after a new explicit T1 authorization and authoritative answers.
No certification claim follows from these offline tests.
