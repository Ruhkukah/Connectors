# Listener-only C2 capture harness

Status: PREPARED_OFFLINE. No T1 access authorized or performed. This harness does
not change the PR #43 gates: regular/multileg mapping and snapshot chronology
REQUIRE_T1_CAPTURE; public_order_id identity requires written MOEX authority;
production C2 remains BLOCKED. No cross-pair atomicity is claimed.

## Build and boundary

Build `plaza2_c2_capture` and `plaza2_c2_preflight_test` with CMake in Release.
The recorder supports little-endian Linux x86_64 and macOS arm64. The Linux CI
artifact `plaza2-c2-capture-linux-amd64` contains the executable, offline oracle
and Python importer. Restore executable permissions after artifact extraction.
Invoke the recorder by its absolute path so its executable hash is unambiguous.
Use a clean checkout/build to associate the embedded Git SHA with the sources.

The recorder links the existing SHA-256 implementation through a separate static
library, not the transaction-capable runtime object. It dynamically resolves only:

```text
cg_env_open cg_env_close
cg_conn_new cg_conn_open cg_conn_close cg_conn_destroy
cg_conn_process cg_conn_getstate
cg_lsn_new cg_lsn_open cg_lsn_close cg_lsn_destroy
cg_lsn_getstate cg_lsn_getscheme
cg_err_getstr cg_env_getcomp_ver
```

There are no publisher handles, order commands, transaction hosts or reconciliation
paths. The fake runtime audits every CGate export, including publisher exports;
tests require every call to belong to this 16-function allowlist and publisher
creation/open/post and transaction command counts to be zero. Symbol inspection
also rejects publisher and ConnectorHost symbols in the capture binary.

## Configuration and later exercises

The following is a template, not a T1 execution instruction. After separate
explicit market-data capture authorization, prepare a private mode-600 config
outside the repository, replacing placeholders with approved values:

```ini
runtime=/ABSOLUTE/SDK/lib/libcgate.so
env=ini=/ABSOLUTE/PRIVATE/cgate.ini
env_file=/ABSOLUTE/PRIVATE/cgate.ini
connection=p2tcp://APPROVED_ACCESS_SERVER:PORT
regular=p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;name=c2_regular
multileg=p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;name=c2_multileg;snapshot.data=multileg_orders;online.data=multileg_orders_log;snapshot.bind=info.trades_rev
ordbook_scheme=/ABSOLUTE/QUALIFIED/ordbook.ini
ordlog_scheme=/ABSOLUTE/QUALIFIED/ordLog_trades.ini
```

Omit `multileg` for regular-only runs. `env_file` must identify any INI used by
`env`, for hashing and credential discovery; keep any referenced runtime sources
under the same controlled evidence custody. Both qualified schema files are
hashed even when explicit listener schemes are unnecessary. Optional
`online.scheme=|FILE|...|CustReplScheme` and
`snapshot.scheme=|FILE|...|CustReplScheme` must match these hashed paths exactly.
Other streams/settings and raw replstate are rejected. No trading/order account
is required. The SDK log is forced to error-only stdout, captured with stderr in
a bounded diagnostic pipe, and redacted before persistence. Configuration and
connection strings and known credential values are redacted; an identified
credential in mandatory raw bytes invalidates the capture instead of changing it.
Never put credentials in listener names or source filenames.

After authorization, use absolute paths and a new evidence directory:

```sh
/ABSOLUTE/BUILD/apps/plaza2_c2_capture \
  --config /ABSOLUTE/PRIVATE/capture.cfg \
  --output /ABSOLUTE/cert/evidence/plaza2/DATE/CAPTURE_ID \
  --duration-ms 60000 --reopen-ms 0 --buffer-bytes 4194304
```

Duration is 1..3600000 ms. SIGINT/SIGTERM also request clean completion.
`--reopen-ms 0` disables restart (CAP-REG-BOOT / CAP-PASSIVE / CAP-MULTILEG).
A positive value, no greater than duration, selects CAP-REG-REOPEN: after the
regular listener reaches ONLINE with the expected descriptor, close/destroy it,
wait that interval, and create/open a fresh listener with empty settings. The
second bootstrap must fit within the overall duration. This is a listener restart;
no network, server, router, TCS, LifeNum or retention manipulation is implemented.
Each pair has separate ordinals, epochs, state and per-table committed callback
frontiers. A reopen preserves the pair's ordinal and increments its local epoch.

Capture completion means transport evidence was persisted, not that bootstrap,
protocol equivalence or certification passed. Candidate outcomes distinguish
SUCCESS_NEGOTIATED, OPEN_REJECTED_WITH_EXACT_ERROR and
OPENED_BUT_UNEXPECTED_DESCRIPTOR. Asynchronous state ERROR has no invented error
code: state/API observations and available redacted SDK diagnostics are retained.
A rejected candidate proves only that attempt failed. Quiet multileg is
NOT_OBSERVED, not unsupported. Natural control events are preserved.

## Binary source evidence, version 1

`capture.bin` is the source. The recorder writes a new `capture.part` in a new
mode-700 directory, appends a footer, fsyncs, renames and makes it read-only.
`capture.bin.sha256` identifies the completed bytes. This protects against
accidental changes; external SHA custody remains necessary for forensic use.
Failed runs retain an incomplete `.part` and never publish `capture.bin`.
`manifest.json` and `metrics.json` are receipts, not substitutes for the trace.

All integers are little-endian. Magic: eight bytes `P2CAP001`. Each frame starts
with a u32 byte count, followed by this fixed 88-byte header and payload/null bytes:

| Field | Encoding |
| --- | --- |
| kind, listener | u32, u32 |
| callback ordinal, poll ordinal, monotonic receive ns, local epoch | four u64 |
| native callback type, native message id | u32, u32 |
| callback table index | u64 |
| revision, owner id | i64, i64 |
| user id | u64 |
| null-map length, payload length | u32, u32 |
| payload, null-map | exact bytes |

Kinds: 1 build/environment JSON; 2 actual OPEN descriptor JSON; 3 raw native
callback; 4 API/state JSON; 5 footer JSON; 6 redacted SDK diagnostic JSON.
JSON blocks describe cold metadata; market messages are not formatted as logs.
Descriptor blocks retain scheme type/features, all table ordinals/ids/names/sizes/
alignment and fields with name/type/size/offset. Unknown tables/control bytes are
preserved. Non-ASCII descriptor bytes use reversible byte escapes. The importer
hashes descriptor blocks and binds records to the captured OPEN/epoch/index.

Metadata contains format/endian/architecture/pointer width, connector Git SHA,
executable/runtime/config SHA-256, source file hashes, sanitized listener definitions
and run settings. The runtime version query result is a separate captured event.
The footer includes a SHA-256 of the prefix and persisted frame/callback counts;
the sidecar hashes the entire file including the footer. Monotonic times annotate
observation; callback ordinals are authoritative within each listener. Poll/time
ordering never proves regular/multileg atomicity.

A single preallocated 4 MiB buffer (configurable 512 bytes..64 MiB) batches
sequential writes at 64 KiB or half capacity, and at idle polls. There is no
per-market-record heap allocation for framing or formatting. The callback owner
also writes chunks; storage backpressure is bounded by the buffer, with no hidden
unbounded queue. Diagnostic buffering is separately bounded at 256 lines of up to
64 KiB. This does not measure loss upstream of callbacks inside the SDK/network.

Metrics count callbacks received, fully flushed records, written bytes, buffer
high-water, overflow, write/descriptor failures, unknown callbacks and first lost
ordinal/listener. Any mandatory-record loss makes CAPTURE_INVALID (exit 2).
The first lost ordinal conservatively identifies the earliest unflushed callback;
metadata-only failures can have ordinal zero. Disk-full may also prevent writing
the failure receipt: exit failure and absence of a finalized trace are authoritative.

## Offline import and analysis

Import only after copying the immutable trace and SHA sidecar into controlled
custody. No CGate runtime or network connection is needed:

```sh
python3 /ABSOLUTE/REPO/tools/plaza2_c2_capture_import.py \
  /ABSOLUTE/EVIDENCE/capture.bin --output /ABSOLUTE/EVIDENCE/derived \
  --oracle /ABSOLUTE/BUILD/tests/plaza2_c2_preflight_test
```

The derived directory must be new. It contains `environment.json`,
`descriptors.json`, `controls.jsonl`, `records.jsonl`, `metrics.json`,
`analysis.json`, `import_manifest.json` and `replay.tsv`. Native receipts remain in
the parent evidence directory. Verification checks SHA, framing, footer/prefix
hash, counts and callback continuity before producing an index. Source bytes are
never rewritten and the hash is rechecked. The manifest retains source, importer,
fixture and oracle hashes; the fixture embeds its source trace hash.

Decoding uses captured descriptors, not source table ordinals. Qualified integer
and decimal fields are decoded without floating point; unsupported fields retain
raw hex and unsupported shapes remain explicitly unresolved. Oracle fixtures are
generated mechanically from those rows, including full field fingerprints for
replay-conflict checks, and run through the existing PR #43 reference model.

Analysis reports actual mappings, unknown tables, snapshot/info/TN/ONLINE/first-log
chronology, info revision bounds and continuation observations (no R+1 assumption),
snapshot generation and LifeNum controls, and independent listener activity.
The compact chronology sample is bounded; full records remain in JSONL and binary.
Continuous/fresh comparisons require the same observed life and committed frontier
across epochs. PASS_CONDITIONAL validates only the captured/model comparison;
missing common frontier is NOT_OBSERVED. Identity authority, actual T1 topology and
universal restart guarantees remain outside this offline proof.

## Fake preflight

`ctest -L c2_preflight` selects six tests, all without publisher execution. The
capture test runs eight fake-native cases: regular/multileg/reopen; permuted
indices; unknown table/control plus LifeNum/ClearDeleted/CLOSE/error; exact
multileg rejection with credential redaction; unexpected descriptor; quiet data;
small-buffer overflow; real OS write-size failure. It also verifies immutable
roundtrip, SHA corruption/truncation rejection and existing-oracle import.
Release and ASan/UBSan execute the same selection. Linux adds LeakSanitizer.
No offline result authorizes T1. Obtain explicit authorization before the first
market-data-only run, then review its observations before any further scenarios.
