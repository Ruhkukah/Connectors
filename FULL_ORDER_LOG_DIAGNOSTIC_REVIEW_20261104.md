# PR70: bounded native-ordering diagnostics — 4 November 2026

## Scope and native evidence

This change is offline preparation on PR70 only. Certified AGGR binaries and PR68 are unchanged. B/C/D are on hold until a valid read-only book is demonstrated. A diagnostic run requires a new owner message, qualify only, no orders, <=5 minutes, an absolute identity-checked TERM deadline and FOL-then-router shutdown. No schedule, automatic start, admission relaxation or new order authority is introduced.

The earlier read-only retry used the original dedicated router profile and full vendor links. SOFTWARE_AUTH resolved; committed REFDATA resolved matching ID1 for the selected ISIN. Composite create/open accepted both restricted schemas. The consumer then rejected a tracked row, with49 recovery cycles in approximately55 seconds. No valid composite ONLINE/book was obtained; zero orders were sent and FOL/router stopped.

Pinned fol.stdout logged exactly49 Open,49 Close,49 ClearDeleted(orders,MAX) and49 ClearDeleted(info,MAX). Its repeated visible ordering is Open -> ClearDeleted orders -> ClearDeleted info -> combined row-validation failure -> Close. It suppresses TN_BEGIN, TN_COMMIT and ordinary orders/orders_log data in its event logger. Their absent log lines are NOT proof of absent native events. StreamData totals by table, transaction counts/order, and first-row size cannot be reconstructed from that log. Native default logs identify schema negotiation and channel operations, but do not provide those missing per-row callback facts. No composite Online or LifeNum appears in the saved sparse event log; ONLINE was never successfully reached.

Read the vendor SDK sample on TEST, without starting it: samples/c/orderbook/ordbook.cpp, SHA-256 ebacad1d36ec087a9d2f04a8a4c6bfc87f94805e7d994762e0d79a959c6374e4. Its callback switch initializes schema/clears on Open (984–989), handles StreamData directly (1005 onward), selects snapshot orders versus orders_log, ignores snapshot replAct!=0 (1017–1025), handles LifeNum/ClearDeleted/Close, and logs ONLINE as snapshot completion (1162–1165). It does not handle TN_BEGIN/TN_COMMIT in that switch, although their names occur in its generic message-name dictionary (275–276). This supports investigating an unframed snapshot; it does not establish the exact ordering of this native run. No vendor source is copied into the repository. The manual's claimed p2repl-only marker table is not independently revalidated by this commit; do not turn the sample or missing sparse logs into a normative protocol guarantee.

## Diagnostics without semantic changes

The tracked orders/orders_log check remains strict and in its existing position after the ISIN filter:

- `row outside transaction` reports table, whether ONLINE has been seen in this listener cycle, event kind and1-based event sequence position.
- `row size` reports table and actual versus expected bytes, independently of the transaction error.

No string construction is added to the successful row-validation path. The first64 delivered events per listener cycle log cycle/position, kind, table name and native data_size only; no field values or raw bytes. A fixed11-counter array records totals for all delivered events and emits one summary on close (including owner-initiated teardown); duplicate native/synthetic closes do not create duplicate summaries. Counts indices0–9 correspond to Open, Close, TN_BEGIN, TN_COMMIT, StreamData, Online, LifeNum, ClearDeleted, ReplState, Timeout;10 is an unknown-kind bucket. Owner-synthetic Close has data_size0. The trace reflects consumer-delivered events; it is not a wire capture.

Before ONLINE, three consecutive listener cycles with the same DecodeFailed row-validation reason make the session terminal. Changing only the row's event position does not hide a repeated reason; table, kind, ONLINE state and actual/expected size remain part of the signature. A different row reason starts at one; a non-row validation failure breaks the streak; an accepted Online clears it. Each cycle contributes at most one strike. Terminal state propagates to both qualify and writer run, exits non-zero, and cannot issue a fourth OPEN. Normal recoverable disconnects/watchdogs keep their previous behavior.

## Shared compiled-source review

| File | Effect with no FOL handler | Proof |
|---|---|---|
| connectors/plaza2_trade/src/cgate_session.cpp | New bounded trace/counter/fatal behavior is reached only through the FOL forwarding handler. Both FOL modes now use it; ordinary AGGR listeners do not. | Unchanged AGGR/runtime/session/host/isolation tests in the full suite; FOL session test additionally verifies the ordinary five-table REFDATA path. |
| protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_runtime.hpp | Adds native_data_size metadata to the event struct. It changes the compiled struct layout; no ABI/binary identity claim. Existing AGGR consumers ignore the field. | Unchanged native-ABI, runtime callback and AGGR bridge tests rebuilt in the full suite. |
| protocols/plaza2_cgate/src/plaza2_runtime.cpp | Copies cg_msg data_size into event metadata before existing dispatch. Existing payload, event ordering, descriptors and admission remain unchanged. | Same unchanged runtime/callback/AGGR tests; FOL trace checks metadata. |
| protocols/plaza2_cgate/src/plaza2_full_order_log.cpp | FOL-only split diagnostics and cycle position/ONLINE metadata; admission unchanged. | FOL semantic/allocation tests reject both unframed and undersized rows and retain commit-only publication. |

These files compile into binaries built from PR70; this is not a change to the certified branch or deployed certified artifacts. Do not merge PR70 into PR68. No certification or AGGR live-equivalence claim follows from the offline suite.

## Regression coverage and release

Fake CGate can now omit snapshot TN_BEGIN explicitly, independent of its ordinary framed positive contract. Unit tests prove the separate transaction/size errors and hidden rejected rows. Session tests prove the64-event cap, three-strike terminal state, no fourth OPEN, stable fatal error, one totals record per close, and ONLINE resetting the streak. Actual-executable fake tests prove both qualify and run exit non-zero with the fatal reason, no native order posts and cleaned command sockets. Existing trading/session/commit/size gates remain in force.

Release/CI receipts and PR metadata identify the exact diagnostic code SHA and the new separate binary SHA-256. Use a new immutable FOL release directory; never overwrite298d9b1e or either certified AGGR binary. The older staged environment still points to the prior binary: a future authorized diagnostic command must explicitly select the new manifest's BIN/SCHEMES and the original full vendor-link router profile. Preserve existing native logs before using any vendor log destination that is recreated at startup. Deployment is preparation only; no native test starts with this commit.
