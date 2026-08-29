# PLAZA II order lifecycle V2.1/V2.2 final review

## Scope and verdict

This document covers the V2.1 correctness pass from authoritative head
`ad29ad5685f9cdfc664a14c244c92d9157aa7146` and the final V2.2 merge-gate
correction from head `1e8b100a65da9f03712bf74762cb97fbb239cbfa` on branch
`codex/plaza2-order-lifecycle-v2`. Both stay inside the existing
transport-neutral, offline lifecycle design. No new live transport, profile,
wrapper, script, C ABI, production path, or generic recovery framework was
added by that lifecycle increment. The follow-on V1 fake-runtime transport and
its single semantic test executable are documented separately.

The offline acceptance gate passes. No production connection, TEST connection,
network command, credential, software key, account secret, broker profile, or
live order was used. The native runner still has no live transport and refuses
`--send-test-order` after validating the exact plan hash.

This verdict is a review gate, not authorization for a live TEST order.

## Authoritative semantics

The implementation was checked against the repository-pinned CGate/P2Gate
material and the current official MOEX CGate 9.3 header:

- `spec-lock/prod/plaza2/cgate_docs/cache/cgate_en.pdf`, publisher and connection
  processing sections: message allocation, post, and free are distinct;
  `CG_PUB_NEEDREPLY` routes an originating `uint32 user_id` to `p2mqreply`;
  `cg_conn_process` timeout means no event, while publisher timeout is not
  generic success.
- `spec-lock/prod/plaza2/cgate_docs/cache/p2gate_en.html`, AddOrder/DelOrder and
  DelUserOrders sections: AddOrder 474 replies with 179 and an `order_id`;
  DelOrder 461 consumes that ID; DelUserOrders 466 with nonzero `ext_id` is the
  documented narrow response to an uncertain Add/Move outcome; deletion timeout
  does not establish deletion.
- The current official CGate distribution header confirms
  `CG_MSG_DATA == 0x110`, `CG_MSG_P2MQ_TIMEOUT == 0x1001`, and the
  `cg_msg_data_t` ABI including `owner_id` and the `user_id` union member:
  <https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/Game/>.

A P2MQ timeout is surfaced as uncertainty with only its originating `user_id`.
The transport can associate it with AddOrder, DelOrder, or exact-ext recovery
through the three fixed, unique user IDs; the generic runtime does not invent an
operation or rejection.

## Corrected lifecycle completion

Reply and replication are asynchronous evidence surfaces. Add-phase polling is
now state-driven:

- factual `Filled` or `Cancelled` replication stops immediately;
- a correlated, non-timeout AddOrder rejection resolves only when no
  contradictory order evidence exists;
- `Working` or `PartiallyFilled` continues until an accepted correlated reply
  supplies `order_id`, that ID matches a replicated public/private ID or alias,
  and the evidence remains consistent;
- an accepted reply arriving first continues until replication appears;
- intermediate empty polls do not discard either surface;
- any deadline without a usable cancellation precondition or terminal state
  invokes reconciliation, even when one evidence surface is already present.

V2.2 makes terminal replication fail closed against inconsistent evidence.
`Filled` or `Cancelled` establishes `market_safe_terminal=true` only while
`evidence_consistent=true`. Identity conflict, contradictory ordinary reply
evidence, or a mismatch retained from before exact-ext recovery converts the
result to `unresolved_orphan_incident`, keeps `market_safe_terminal=false`, and
retains every ext/add/cancel/recovery lock. Evidence consistency is sticky:
once false, no later observation or recovery response can restore it in the
same run. Restart-time reconciliation is the future clearing mechanism.

For the MOEX TEST contour, `FORTS_USERORDERBOOK_REPL` and
`FORTS_TRADE_REPL` are independent surfaces, not two replicas of one order.
USERORDERBOOK contributes only the current own-order snapshot used by the
pre-send active-order census. Lifecycle observations and ordinary AddOrder
reply/replication identity checks use the TRADE order surface (plus its own
deals), and POS-anchored position replay uses TRADE independently. A TEST
USERORDERBOOK-versus-TRADE difference in identity, existence, amount/rest,
fill/lifecycle state, row count, or revision is not lifecycle inconsistency and
must not fail position qualification. This exception does not relax any
reply/replication check for an order actually submitted by the connector.

DelOrder is still encoded only from the accepted AddOrder reply ID. Public or
private replication IDs are never guessed into DelOrder. Conflicting ordinary
replies, reply/replication ID mismatch, contradictory rejection/order evidence,
and changing replicated identities set `evidence_consistent=false`.

When AddOrder may have processed and an accepted AddOrder ID remains unavailable,
the controller may submit exactly one transport-neutral recovery operation:
DelUserOrders constrained to this run's nonzero `ext_id`, client, side, common
order class, base contract, `isin_id`, instrument mask, and broker context. It
never submits another AddOrder and never exposes broad cancellation. After that
request it continues polling and reconciliation; a delete reply or P2MQ timeout
does not become factual cancellation.

## Canonical pre-send plan

The lifecycle plan is a static `moex.plaza2.authorized_order_intent.v1`. It
hashes the encoded AddOrder and prevalidated exact-ext recovery payloads plus
the non-secret static intent and policy:

- price, side, quantity, and instrument identity;
- smoke-policy version, policy SHA-256, and maximum distance ticks;
- profile/environment fingerprints and all unique lifecycle IDs.

Broker/client/account values are not written. Their exact AddOrder and recovery
values remain committed through encoded payload SHA-256 fingerprints.
Dynamic BBO, session, refdata, limits, and local-age observations are persisted
as reviewed evidence but are intentionally excluded from the authorization
hash. The concrete TEST transport derives a fresh, atomically persisted
execution-safety receipt immediately before AddOrder. A changed market therefore
cannot silently authorize a different order or reprice the authorized intent.

The misleading price-versus-`max_notional` comparison and dead `max_quantity`
surface were removed. The first smoke remains hard quantity one, limit-only,
tick-aligned, fresh two-sided, passive/non-marketable, within maximum BBO
distance, and backed by applicable refdata/session/limits evidence.

## Atomic local journal and degradation

`moex.plaza2.order_run_journal.v2` is an atomic local journal, not a claim of
power-loss durability. Each update writes and flushes a same-directory temporary
file, then atomically renames it over the published path. The previous complete
file remains until rename succeeds. There is no file `fsync` or parent-directory
`fsync`, so persistence through kernel, filesystem, or power failure is not
claimed.

Every intermediate persistence failure is retained in memory as
`journal_degraded=true`. After AddOrder is possibly sent or posted, journal
failure does not stop polling, reconciliation, DelOrder, or exact-ext recovery.
Results independently report:

- `market_safe_terminal` — consistent replication proved Filled/Cancelled, or
  the command was definitely not sent/rejected before an order could exist;
- `journal_ok` — the final state persisted and no earlier journal write failed;
- `evidence_consistent` — reply and replication identities/outcomes agree.

Any degraded run and any inconsistent terminal incident retains its
ext/add/cancel/recovery identifier locks, including after factual terminal
replication. `orphan_incident_written` is true only when the final orphan state
was actually published successfully.

## Runtime capability gate

The read-side runtime ABI remains the compatibility baseline. Publisher symbols
are loaded optionally and do not make a read-only installation incompatible.
The separate `trading_capable` report requires connection processing, untyped
reply-listener lifecycle, and all publisher lifecycle/message functions. Missing
trading symbols are listed explicitly for a future concrete trade transport.

## Offline verification

The existing lifecycle scenario executable now additionally covers:

- Working replication before a later accepted reply;
- accepted reply before later Working replication;
- accepted reply after an intermediate empty poll;
- reply-only timeout with reconciliation finding Working;
- replication-only timeout with reconciliation finding the accepted reply;
- terminal replication before a reply;
- replication without an accepted ID using the exact encoded ext recovery;
- Add/recovery P2MQ timeouts remaining unresolved rather than cancelled;
- BBO, freshness, session, refdata, limits, policy, and instrument-context plan
  hash sensitivity;
- journal failure after Add post, while recording a reply, while publishing a
  final orphan, and after market-terminal cancellation;
- full fill carrying an identity conflict;
- cancelled replication contradicting the ordinary AddOrder reply identity;
- terminal cancellation after exact-ext recovery with pre-existing inconsistent
  evidence.

The existing fake-runtime adapter test round-trips ordinary `CG_MSG_DATA`, an
AddOrder `CG_MSG_P2MQ_TIMEOUT` with user ID 704, and a DelOrder timeout with user
ID 705. The runtime probe test checks both complete trading capability and that
`cg_pub_post` is not a read-side compatibility requirement.

Verification on AppleClang 17 / Debug / Ninja:

| Check | Result |
|---|---:|
| Full CTest | 154/154 pass |
| ASan+UBSan changed-target set | 4/4 pass |
| Native offline plan/privacy check | pass |
| `git diff --check` | pass |
| Network or live order activity | none |

The sanitizer set was the lifecycle scenarios, runtime adapter, runtime probe,
and offline no-send guard. It used
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` because Apple ASan does not support
leak detection, plus `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.

## Delta from authoritative V2 head

| Metric | `ad29ad5685` | V2.1/V2.2 final | Delta |
|---|---:|---:|---:|
| CMake targets | 226 | 226 | 0 |
| CTests | 154 | 154 | 0 |
| Tracked files changed | 0 | 10 | +10 |
| Lines added | 0 | 1,242 | +1,242 |
| Lines removed | 0 | 380 | +380 |

No CMake target or CTest was added; existing scenario/runtime targets were
extended as requested.

## Remaining blockers before a live TEST order

The separate V1 transport review now covers the offline fake-runtime
composition of the publisher, timeout/reply mapping, private replication, and
reconciliation APIs. The following gates still remain before any live TEST
order:

1. Source plan evidence directly from authoritative committed refdata, session,
   AGGR20, and limits snapshots rather than operator-supplied offline values.
2. Validate the installed broker TEST runtime/scheme and reply decoders without
   placing credentials or endpoints in Git.
3. Define restart-time orphan reconciliation and operational journal ownership,
   permissions, retention, alerting, and Linux durability requirements.
4. Review a newly generated plan and require a separate explicit authorization
   before any one-order TEST exercise.

Stop here. No VPS run, network session, live TEST submission, production work,
or new protocol phase is part of V2.1/V2.2. The V1 fake-runtime transport is
offline-only and does not authorize a live TEST order.
