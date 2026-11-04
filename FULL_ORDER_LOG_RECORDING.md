# Full Order Log recording and offline DBN export

Recording is opt-in in the separate FOL executable. It does not change the certified AGGR executable, subscriptions or installation. Build prerequisites: C++20 and Zstd development files (`libzstd-dev` on Debian/Ubuntu; `zstd` on Homebrew). Zstd is linked into the FOL recorder/archive targets only. The FOL executable now also requests the restricted public OI listener `p2repl://FORTS_DEALS_REPL;tables=deal` on its existing connection (no extra router client). This source change is not a live-start authorization.

Add these options to an otherwise authorized FOL `plaza2 qualify` or `plaza2 run` invocation:

```text
--capture-dir /absolute/path/to/a/new-session-directory
--capture-max-bytes 4294967296
--capture-min-free-bytes 10737418240
```

The directory must not exist. Defaults are a 4 GiB archive budget and a 10 GiB free-space reserve. The selected instruments are the existing `--isin-id` list; recording does not broaden any listener. `cmd` and `--cancel-only` refuse capture options. These flags are not an authorization to start a connection or trade.

The owner copies rows into a preallocated 32-block queue, each block up to 1 MiB. A worker compresses independently decodable Zstd level-1 blocks, rotates segments before 32 MiB and syncs completed writes every second and at rotation/close. The owner polls partial-block flushes every 200 ms, including quiet periods. There is no silent-drop or deletion policy. A full queue, write/sync error or disk limit is an explicit failure. Qualification stops nonzero. A writer engages its kill switch, invalidates FOL authority and requests the existing SIGTERM cancel drain; final exit is nonzero. Normal operator cleanup rules still apply if cancellation or transport fails. Do not treat an incomplete archive or a cached order state as proof of flatness.

## What is retained

Complete application-visible selected-instrument `orders` and `orders_log` payloads and null maps, before exclusions by the book rules. Selected-instrument `FORTS_DEALS_REPL.deal` rows (including `xpos`) and its control callbacks are also recorded. FOL `info`, `sys_events`, OPEN schema descriptors, transaction markers, ONLINE, Close, LifeNum, ClearDeleted and replication-state callbacks are retained in order. The manifest contains the instrument IDs and source revision. Records include source/capture ordinals, UTC/monotonic arrival timestamps and the decoded control metadata. There is no pointer serialization or network-packet capture.

This recorder wraps the **four-table composite FOL listener and the one-table public DEALS OI listener**. Other listeners, including REFDATA and private/account streams, are not raw-recorded. The archived numeric ISIN universe and negotiated FOL schemas are sufficient for standalone book replay; symbol names and the trading/session reconciliation state must not be inferred from this archive. Recording itself adds no further subscription beyond the explicit FOL/OI inventory.

Version 2 `.folz` segments use a 32-byte segment header, independent Zstd chunks with a 32-byte size/count/sequence/checksum header, and 88-byte record envelopes. The OPEN embeds its table and field descriptors in the checksummed stream. Source ordinals can have gaps only because unselected order rows were omitted. A final end record is written and synced only on explicit clean finish after host shutdown. The reader checks segment continuity, frame checksums, record lengths/ordinals and the clean end. Missing, damaged, aborted or captures containing invalid raw rows are refused. Listener-error markers withdraw the corresponding generation during replay; they are distinct from archive corruption. No power-loss recovery guarantee is inferred from an ordinary sync test. Keep the original archive when an export is refused.

## Offline commands

```sh
moex_fol_archive replay /path/to/session
moex_fol_archive export-dbn /path/to/session /path/to/new-output.dbn
```

These commands do not initialize CGate, load credentials or open a router connection. Replay uses the same FOL book implementation and preserves snapshot/commit visibility. The report gives callback/row/valid-commit counts and the order count at the last valid committed point; Close can invalidate the final live view afterward.

Export writes an exclusive temporary file and publishes the requested output only after full archive validation and file sync. Existing outputs are never overwritten. Output is uncompressed DBN version 3, mixed MBO (56 bytes) and Statistics (80 bytes) records, using the local dataset label `MOEX.FOL`, numeric MOEX ISIN instrument IDs, and publisher 0 (unassigned/local, **not Databento-supplied market data**). DBN metadata has no raw-symbol mapping. Use the original capture for provenance and MOEX replication details.

The exporter replays the real admission rules, emits an initial clear plus snapshot adds only when the book is published, and subsequently emits committed A/M/C book changes. M is the absolute new remaining quantity; C removes the previous remaining quantity. LAST marks the last change for each instrument in a committed batch. Invalidation emits a clear marked MAYBE_BAD_BOOK; a new valid generation starts with a new snapshot. It does not emit unpublished snapshot fragments. The offline exporter inspects changed order IDs rather than cloning the full book on each ordinary commit. Full enumeration is needed only for a new snapshot or a revision-bounded clear. Transaction buffering is capped at one million output changes.

Prices are rescaled exactly from MOEX 1e-5 to DBN 1e-9 units. Unsupported quantities, prices or export-sequence overflow stop export without publishing output. No narrowing, saturation or wraparound is permitted. DBN receive timestamps use the captured commit/ONLINE receipt time at which normalized data becomes visible; raw per-row receipt times remain in the lossless archive. Ordinary updates retain raw `moment_ns`, while reconstructed book snapshots use the instrument's committed snapshot time. Read records in file order; no shared exchange revision or global event-time ordering between the two streams is asserted. Missing timestamps use DBN's undefined sentinel. `ts_in_delta` is undefined, since the feed does not supply the matching-engine send time. DBN sequence is an export ordinal, not MOEX `replRev`.

## Deliberate differences from the lossless archive

MBO is a normalized book view. It omits source table/replication identities and revisions, `sess_id`, xstatus flags, original quantity, iceberg initialization fields, native control messages, schema/null maps and raw bytes. Excluded rows remain in the lossless capture but are omitted from MBO. `id_deal`/`deal_price` are not exported as trade prints: a fill affects remaining quantity through M or C. Do not calculate trade volume or aggressor flow from these changes.

Open interest comes from the separately restricted `FORTS_DEALS_REPL.deal.xpos` source. The consumer validates `xpos` as `i8` plus required session, instrument, trade identity, replication and nanosecond-time fields at OPEN, and reads selected-instrument values only. The first completed snapshot supplies the latest observed OI per instrument. After ONLINE, **every live OI observation in each completed transaction** is exported, including multiple changes within one batch. Interrupted/uncommitted observations are hidden. Revision-bounded housekeeping does not undo the latest published OI observation; a generation change or Close withdraws live availability until a new snapshot. There is no fabricated zero or derivation from orders/fills. Offline DBN Statistics uses `stat_type=9`, raw signed 64-bit `quantity`, and `ts_ref` equal to the observation's `moment_ns`; values conflicting with the DBN undefined sentinel are refused. Consumers must not infer continuous availability by forward-filling observations through a source disconnect; the original archive preserves those lifecycle boundaries. Initial historical trades are preserved raw, but OI normalization starts with the snapshot's latest value rather than replaying all pre-ONLINE history.

DBN layout references: [official MboMsg](https://github.com/databento/dbn/blob/main/rust/dbn/src/record.rs), [metadata encoder](https://github.com/databento/dbn/blob/main/rust/dbn/src/encode/dbn/sync.rs), [flags](https://github.com/databento/dbn/blob/main/rust/dbn/src/flags.rs).

## Load and retention limits

A successful offline load is implementation evidence, not native R7 qualification or a production capacity guarantee. The earlier prototype's 300k/s workload was stopped by a shared-host CPU-pressure guard while pinned to one low-priority core; this is not a demonstrated transport loss, but sustained 300k/s remains unproven. Choose a dedicated capacity/retention budget and measure actual traffic, burst backlog, sync tails and downstream clients before production. The archive budget bounds growth; it does not reserve filesystem capacity against other processes. No moving release pointer or deployment is part of this change.
