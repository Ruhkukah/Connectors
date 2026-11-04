# Full Order Log recording and offline DBN export

Recording is opt-in in the separate FOL executable. It does not change the certified AGGR executable, subscriptions or installation. Build prerequisites: C++20 and Zstd development files (`libzstd-dev` on Debian/Ubuntu; `zstd` on Homebrew). Zstd is linked into the FOL recorder/archive targets only. Only with `--open-interest` (default off), the FOL executable requests the restricted public OI listener `p2repl://FORTS_DEALS_REPL;tables=deal` on its existing connection (no extra router client). This source change is not a live-start authorization.

For OI, explicitly add `--open-interest`. Without it, no public DEALS listener opens and the listener inventory matches the installed pre-recorder FOL release. The flag is refused for cmd/cancel-only.

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

This recorder wraps the **four-table composite FOL listener and, when enabled, the one-table public DEALS OI listener**. Other listeners, including REFDATA and private/account streams, are not raw-recorded. The archived numeric ISIN universe and negotiated FOL schemas are sufficient for standalone book replay; symbol names and the trading/session reconciliation state must not be inferred from this archive. Recording itself adds no further subscription beyond the explicit FOL/OI inventory.

Version 2 `.folz` segments use a 32-byte segment header, independent Zstd chunks with a 32-byte size/count/sequence/checksum header, and 88-byte record envelopes. The OPEN embeds its table and field descriptors in the checksummed stream. Source ordinals can have gaps only because unselected order rows were omitted. After host shutdown, an end record carries clean (0), host-nonzero (1), or capture-failed (2) status and the host exit code. A 40-byte `stop-status.folend` is reserved/synced before startup; finalization updates its identity/checksum/status independently of the data queue, including when that queue has failed. This permits a budget/queue failure to record its stop status without appending after a missing data chunk. A physical disk/sync failure can also prevent this update; it remains incomplete, never claimed clean. The reader checks segment continuity, frame checksums, record lengths/ordinals and the clean end. Missing, damaged, aborted or captures containing invalid raw rows are refused. Listener-error markers withdraw the corresponding generation during replay; they are distinct from archive corruption. No power-loss recovery guarantee is inferred from an ordinary sync test. Keep the original archive when an export is refused.

## Offline commands

```sh
moex_fol_archive replay /path/to/session
moex_fol_archive export-dbn /path/to/session /path/to/new-output.dbn
```

With explicit `--allow-incomplete`, both commands read only the contiguous verified prefix through the last complete, checksummed and structurally validated chunk; they never skip a bad chunk and resume later. Strict rejection remains the default. Reports include `truncated`, `last_good_sequence`, `end_status` and `host_status`; retain this JSON beside derived DBN. The last-good sequence is an archive record ordinal, not an exchange revision. A listener error withdraws its generation; only a fresh committed snapshot can restore it. DBN emits R with MAYBE_BAD_BOOK, followed by a snapshot when available. At an incomplete EOF, uncommitted deltas are discarded and a final MAYBE_BAD_BOOK reset withdraws the last view. Row/schema/field validation is never relaxed; malformed source data still fails. No clean-end, usable manifest, or first verified chunk means no replayable prefix.

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

The idle worker wait is 1 ms. A SIGTERM-stopped qualify returns 0 after orderly shutdown and writes a clean end; fake executable regression covers this. A writer recorder-failure regression has two Working orders, a bounded-record capture failure, kill switch, private cancel confirmations for both orders, completed drain, and exit 3. This is offline evidence, not proof of native emergency cancellation. DBN trades and a REFDATA symbol sidecar remain future work; the export still makes no trade-volume/aggressor-flow claim.

## COMMON source evaluation (no subscription switch)

The vendor SPECTRA9.12.0 `cgate/scheme/SPECTRA9.12.0/common.ini`, section `[table:CustReplScheme:common]`, declares `field=xpos,i8` (current open positions), with `sess_id,i4` and `isin_id,i4`. The checked vendor archive SHA-256 is `73fb651459fdb1420f82f684e7c2fc3dbaa20dac085f9ff83fdc38fc35e16d61`. COMMON provides per-instrument statistics rather than replaying every session deal, making it the candidate for cheaper startup and quiet-instrument OI. Its update cadence and native T0 OPEN schema still need validation; this check used the vendor bundle, whose header says prod, without a native connection. No COMMON listener is opened by this change. Switching source requires separate review and questionnaire changes.
