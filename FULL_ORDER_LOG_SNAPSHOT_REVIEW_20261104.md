# PR70: hidden unframed composite snapshots — 4 November 2026

This implements the approved snapshot admission change after758f0c76 demonstrated Open, MAX orders/info clears and128-byte snapshot rows without TN_BEGIN. PR68 and certified AGGR remain frozen. No native run starts with this commit.

## Phase contract

Before ONLINE, orders/info/orders_log/sys_events accept framed or unframed rows into hidden staging. Table/index/byte checks run independently of framing; consumed field/type/null checks remain. Info requires publication_state0/1 and nonnegative trades_rev/trades_lifenum. The ISIN filter still precedes nonessential per-order field decoding.

Pre-ONLINE TN_COMMIT never publishes levels or invokes the publication callback. It preserves valid optional snapshot info/life metadata. CGate ONLINE is the completeness authority: unframed snapshots, including empty snapshots without any info callback, publish once at ONLINE. An optional publication_state=0 callback does not veto ONLINE. Optional framing may finish after ONLINE without publishing the same snapshot twice. Fully framed snapshots retain their existing next-TN_COMMIT publication path. Rejected rows unbind the book, so a subsequent ONLINE cannot bypass a fresh validated OPEN. MAX-clear/life reset and fresh-snapshot protections remain.

After ONLINE, every supported table row requires a transaction, including filtered instruments. Size checks run first, so short unframed rows report actual/expected size. Online updates remain commit-atomic. Close, error, unexpected LifeNum and MAX orders/orders_log/info clears discard staged/public state. The snapshot-life versus info-bound log-life distinction is retained, including valid pending pre-ONLINE info. Session/liveness/clock/order-entry gates remain unchanged.

## Diagnostics and terminal policy

Keep the first64 metadata-only events per cycle and add the first32 events after accepted ONLINE. Totals include pre-ONLINE table counts and first/last info positions. The ONLINE record includes elapsed time and complete table counts; no field values. Qualify signals its first valid commit; qualify/write metrics include book_valid.

Pre-ONLINE strikes reset at accepted ONLINE. A separate post-ONLINE row-failure streak survives bootstrap ONLINE on reopen, preventing an endless loop of online framing failures. It resets only after a successful framed post-ONLINE data transaction commits; an empty bootstrap commit does not hide a recurring error. Both modes terminate non-zero after three matching row failures, ignoring only event position.

## Comparison probe

The composite Observer accepts the same hidden unframed snapshot and preserves pending info across optional snapshot TN pairs. It publishes the composite comparison state at ONLINE only if the production book is valid. An exact comparison witness additionally requires an exposed, validated life/revision anchor; ONLINE alone does not invent that anchor. Without one, unanchored_commits increments and the comparison remains UNALIGNED, even with a valid production book. Raw p2repl ORDBOOK remains strictly transactional. Post-ONLINE composite rows are strict. Errors discard comparison witnesses. Existing LastRec, exact-revision alignment, administrative fences and generation rules remain; no unaligned comparison is guessed.

## Tests and shared code

Coverage includes the exact observed prefix (36 filtered rows, tracked row at position40), followed by completed info and ONLINE; mixed framing; hidden snapshot commits; interrupted/empty/incomplete snapshots; negative trades_rev; independent row-size refusal; ONLINE inside an open optional TN pair; atomic online updates; post-ONLINE three-strike stop despite bootstrap ONLINE; first32 trace bound; and the probe's snapshot/empty/interruption/strict-online paths. Malformed-snapshot actual-executable tests retain non-zero qualify/run exits and socket cleanup with zero fake broker posts.

Shared compiled changes are in cgate_session.cpp and the FOL book. The prior native_data_size runtime ABI change is unchanged. New trace/counters/row guards remain dormant without a FOL handler. Unchanged AGGR/runtime/session/host tests are required; this makes no ABI/binary/live-latency equivalence claim for a trading binary rebuilt from PR70. Do not merge into PR68.

## Release and next run

The exact source/binary SHA and CI results are in the new immutable release manifest and PR receipt. Preserve298d9b1e and758f0c76. Future commands must explicitly select the new BIN/SCHEMES; old staged environments are not release selectors.

A native qualify-only run needs a separate owner message, no orders, <=10minutes, an absolute identity-checked TERM deadline, original full vendor links, and FOL-then-router shutdown. Report time to ONLINE, snapshot row counts by table, info position, online framing, book validity/lag and strikes/exit. No native run occurs during this implementation.

B is deferred to5November around08:00, after a valid read-only book and the unchanged3-minute writer/session gate; A can run that morning. C/Kairos is proposed for5November10:20–11:20. D stays held. Every block needs separate authority; no timers/wakeups. Today's proposed diagnostic slot is14:05–14:50; avoid14:55–15:10. No certification/R7 claim.

## ONLINE completeness correction after native f9f50463

The read-only f9f50463 run reached ONLINE in three cycles with 7,141 orders rows each, but zero info callbacks. The old application completeness check then kept the book invalid and caused watchdog reopens. This correction removes that application-only info-callback requirement; it does not remove info from the restricted snapshot scheme. Native validity with this correction is pending a separately authorized run.

Offline vendor verification used SDK9.9.1853 `docs/cgate_en.pdf`, SHA-256 `3654745fddecfa90547bb3ccab95001bdf27bcf44a1513433e61daf5a57c82c1`, printed pages33–34. The cg_lsn_new p2ordbook section requires orders/info in the snapshot scheme and documents snapshot.bind as the table/revision field used to bind streams, default `info.trades_rev`. The SDK orderbook sample handles orders/orders_log/sys_events and treats ONLINE as snapshot completion without application info-row state. Sample `samples/c/orderbook/ordbook.cpp` SHA-256 `ebacad1d36ec087a9d2f04a8a4c6bfc87f94805e7d994762e0d79a959c6374e4`. This verifies the documented internal binding contract, not proprietary library implementation or a promise that info is never delivered.

Additional regressions cover no-info unframed and empty snapshots, duplicate ONLINE, optional publication0, invalid optional info followed by ONLINE, fake-native info consumption with the descriptor retained, and valid-but-unanchored probe output. The DTC epoch regression now captures the actual revision rather than assuming revision2; its negative control still disables epoch discrimination and must fail at the same-revision recovery assertion. No DTC production code changes.

This correction changes only FOL book/probe behavior. AGGR tests run unchanged, but no rebuilt PR70 trading binary is certified. No router/FOL process starts during implementation or release deployment.
