# PR70: hidden unframed composite snapshots — 4 November 2026

This implements the approved snapshot admission change after758f0c76 demonstrated Open, MAX orders/info clears and128-byte snapshot rows without TN_BEGIN. PR68 and certified AGGR remain frozen. No native run starts with this commit.

## Phase contract

Before ONLINE, orders/info/orders_log/sys_events accept framed or unframed rows into hidden staging. Table/index/byte checks run independently of framing; consumed field/type/null checks remain. Info requires publication_state0/1 and nonnegative trades_rev/trades_lifenum. The ISIN filter still precedes nonessential per-order field decoding.

Pre-ONLINE TN_COMMIT never publishes levels or invokes the publication callback. It preserves valid snapshot info/life metadata. A complete unframed snapshot publishes once at ONLINE, requiring snapshot_seen and validated completed info. Empty completed snapshots publish; incomplete info remains invalid and requires a fresh snapshot. Optional framing may finish after ONLINE without publishing the same snapshot twice. Fully framed snapshots retain their existing next-TN_COMMIT publication path.

After ONLINE, every supported table row requires a transaction, including filtered instruments. Size checks run first, so short unframed rows report actual/expected size. Online updates remain commit-atomic. Close, error, unexpected LifeNum and MAX orders/orders_log/info clears discard staged/public state. The snapshot-life versus info-bound log-life distinction is retained, including valid pending pre-ONLINE info. Session/liveness/clock/order-entry gates remain unchanged.

## Diagnostics and terminal policy

Keep the first64 metadata-only events per cycle and add the first32 events after accepted ONLINE. Totals include pre-ONLINE table counts and first/last info positions. The ONLINE record includes elapsed time and complete table counts; no field values. Qualify signals its first valid commit; qualify/write metrics include book_valid.

Pre-ONLINE strikes reset at accepted ONLINE. A separate post-ONLINE row-failure streak survives bootstrap ONLINE on reopen, preventing an endless loop of online framing failures. It resets only after a successful framed post-ONLINE data transaction commits; an empty bootstrap commit does not hide a recurring error. Both modes terminate non-zero after three matching row failures, ignoring only event position.

## Comparison probe

The composite Observer accepts the same hidden unframed snapshot and preserves pending info across optional snapshot TN pairs. It records the exact validated cutover at ONLINE only if the production book is valid. Raw p2repl ORDBOOK remains strictly transactional. Post-ONLINE composite rows are strict. Errors discard comparison witnesses. Existing LastRec, exact-revision alignment, administrative fences and generation rules remain; no unaligned comparison is guessed.

## Tests and shared code

Coverage includes the exact observed prefix (36 filtered rows, tracked row at position40), followed by completed info and ONLINE; mixed framing; hidden snapshot commits; interrupted/empty/incomplete snapshots; negative trades_rev; independent row-size refusal; ONLINE inside an open optional TN pair; atomic online updates; post-ONLINE three-strike stop despite bootstrap ONLINE; first32 trace bound; and the probe's snapshot/empty/interruption/strict-online paths. Malformed-snapshot actual-executable tests retain non-zero qualify/run exits and socket cleanup with zero fake broker posts.

Shared compiled changes are in cgate_session.cpp and the FOL book. The prior native_data_size runtime ABI change is unchanged. New trace/counters/row guards remain dormant without a FOL handler. Unchanged AGGR/runtime/session/host tests are required; this makes no ABI/binary/live-latency equivalence claim for a trading binary rebuilt from PR70. Do not merge into PR68.

## Release and next run

The exact source/binary SHA and CI results are in the new immutable release manifest and PR receipt. Preserve298d9b1e and758f0c76. Future commands must explicitly select the new BIN/SCHEMES; old staged environments are not release selectors.

A native qualify-only run needs a separate owner message, no orders, <=10minutes, an absolute identity-checked TERM deadline, original full vendor links, and FOL-then-router shutdown. Report time to ONLINE, snapshot row counts by table, info position, online framing, book validity/lag and strikes/exit. No native run occurs during this implementation.

B is deferred to5November around08:00, after a valid read-only book and the unchanged3-minute writer/session gate; A can run that morning. C/Kairos is proposed for5November10:20–11:20. D stays held. Every block needs separate authority; no timers/wakeups. Today's proposed diagnostic slot is14:05–14:50; avoid14:55–15:10. No certification/R7 claim.
