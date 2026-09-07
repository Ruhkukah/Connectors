# PLAZA II offline certification tranche

Completed the authorized B, C1, D0, system-reply/rate and narrow AGGR20 recovery work. **Not certification-ready; C2 remains stopped.**
No T1 access and no exchange orders. Only fake/local publishers and a network-disabled vendor SDK environment were exercised.
The FTP source-access problem was resolved; no user fetch is needed.

## Revision and review stack

- Main/base: `22a9dfd0947ccde4645696974e1270099491e2c6` (remote main reverified unchanged).
- Phase A: `669a16de16bd09d3ef859b7a99b221cfa6b353b0`.
- Locally tested implementation: `20ab52ed58d84d3295fe63309992c287c4516ed8` before the CI stack rebase.
- Rebased implementation: `2d6ccb9cabccbe6e34dc1cf897c31008648fe02d`; its production/test source trees are byte-identical to the locally tested implementation.
  The only additional implementation-stack change is the CI sanitizer build command. Final documentation follows that head.
- Worktree: `/Users/pavel/CSharp/MoexConnector-cert-a`. Original workspace and the user-supplied Archive.zip were preserved.

| Phase | Commit | Draft PR |
| --- | --- | --- |
| B | `613bf94` + `41056fc` | [38: full-book scheme qualification](https://github.com/Ruhkukah/Connectors/pull/38) |
| C1 | `ecaaa4c` | [39: raw anonymous ORDLOG](https://github.com/Ruhkukah/Connectors/pull/39) |
| D0 | `cdf3054` | [40: raw throughput harness](https://github.com/Ruhkukah/Connectors/pull/40) |
| CERT-SYS | `05649db` | [41: system replies and publisher rate](https://github.com/Ruhkukah/Connectors/pull/41) |
| AGGR-REC | `2d6ccb9` | [42: AGGR20 lifecycle recovery](https://github.com/Ruhkukah/Connectors/pull/42) |

The stack is B -> C1 -> D0 -> CERT-SYS -> AGGR-REC. D0 is independently reviewable after C1. PR 38 includes Phase A traceability.
GitHub's first sanitizer runs selected the new tests but did not build their binaries because of a stale explicit target list.
The Phase B branch now builds the configured sanitizer targets, and downstream branches were rebased onto that fix.
This was a missing-executable CI failure, not a sanitizer finding. Nothing was merged. The old draft PR 24 was left unchanged. No execution re-arming or C ABI expansion occurred.

## Official evidence and differences

The FTP server's Russian Trusted Sub CA root was missing locally. The official root was fetched over normally verified HTTPS,
then supplied through a request-local CA file. Chain, hostname and expiry verification remained enabled; no global trust change.
The exact public INIs also match those in the downloaded official 9.9.1853 SDK.

All ten public tables and 135 field definitions match the repository's reviewed names/order/types. No material logical schema
change triggered the requested review stop. Native sizes/offsets/packing were previously unfrozen and are now captured from
SDK schemetool-generated headers and x86-64 sizeof/offsetof probes. Current DDS is 990.1.6.42752; the older runtime snapshot
recorded 42744. Its deployment scheme fingerprint was not silently replaced. The SDK/header/library hashes match the prior lock.

The complete per-table/per-field manifest is [wire.json](../../../spec-lock/test/plaza2/public99/wire.json).
Sources actually used and SHA-256 values:

| Source | SHA-256 |
| --- | --- |
| `Scheme/9.9/ordLog_trades.ini` | `0d35d8671d7c3668a95b359e21269f8a852c387e6a1ac9e7dff154b9c2b99676` |
| `Scheme/9.9/ordbook.ini` | `9f39898346bf9580b88feadfc4a7578bd853a884393e2c72c481133687856238` |
| `Scheme/9.9/forts_scheme.ini` | `f74726c9e86a5229732c55cd1b59f1822d20df92cb3d039dd871b67567dc2f3d` |
| `Scheme/9.9/forts_messages.ini` | `99cba03ef3fa9ffdc10c7da30378b69d6bd02e7093d9e64ffc4a27b94999eaa9` |
| `docs/cgate_en.pdf` | `fef822098def1603d56cb4a53fb0ad3080535853215fe9dee25ff46c7f2b37fd` |
| `docs/p2gate_en.html` | `dd3d155aa7fae9f56b7c0b6e782fe38277c3cc332389ade69113d8be06b7b213` |
| `cgate_linux_amd64-9.9.1853.zip` | `49da634749203a919e69132594198348652acb5e731416c8a39bb34969364ce2` |
| SDK `include/cgate.h` | `b057b537034b23e960f27477dab6738ba5d751ac698287a40d8f95ba7a1ef78f` |
| SDK `lib/libcgate.so` | `f63e726a8482b793c3af755a8dc2b9ebb5cd727d88fb58ebb3fe9704a155ce6f` |

The relative source locations above are under
[MOEX CGate TEST](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/).
The wire manifest records each full URL, TLS provenance, SDK sample and schemetool hashes.

ORDLOG implemented: `orders_log`, `multileg_orders_log`, `heartbeat`, `sys_events`.
ORDBOOK verified: `orders`, `multileg_orders`, `info`, `orders_currentday`, `multileg_orders_currentday`, `info_currentday`.
The generated native header includes an assertion for every table size and field offset. Exact BCD conversion was compared with
native `cg_bcd_get` for 100,004 deterministic cases: zero failures. Timestamp milliseconds, unsigned moment_ns, full integer
quantities and unknown payload flags are retained without floating point. Absent public fields such as ext_id are not invented.

## Handoff and raw recovery

The public composite URL is `p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL`.
The documented default binding is `info.trades_rev`; CGate owns the handoff. ONLINE marks transition after initial replication;
TN_COMMIT establishes transaction consistency. Internal buffer implementation/capacity is not documented and is not guessed.
The manual limits cg_lsn_open replication settings to p2repl, so no raw replstate optimization is claimed for p2ordbook.
Composite recovery starts fresh. Multileg pair coordination and negotiated composite callback fixtures remain C2 gates.
See the [twelve-question contract](../../plaza2/P2ORDBOOK_ORDLOG_RECOVERY_9_9.md).

C1 uses the existing connection/listener and callback dispatcher, with complete scheme verification at every OPEN. Raw events
carry exact wire/null bytes, table identity, service fields, life/generation, receive sequence and poll timestamp. All four tables
are delivered through preallocated bounded native queues; there is no per-record string allocation, managed call or clock syscall.

Mandatory records become visible only after TN_COMMIT. A checkpoint is eligible only after mandatory acknowledgement, with no
incomplete transaction or failure. No durable store was added. New-process startup is fresh history; a drained same-process
reopen can pass the exact eligible opaque token and retain replay. Optional overflow marks the subscription incomplete with its
first lost sequence. Mandatory overflow fails health and blocks later tokens. LifeNum invalidates the raw generation; numeric
revision jumps are distinct from proven queue/callback loss. Consecutive duplicates are labeled and retained; history replay
is legal, and conflicting online revisions fail closed. Details: [raw C1 contract](../../plaza2/RAW_ORDLOG_C1.md).

## Certification fixes

99/100 pass both live-bridge family checks with command correlation and raw diagnostics. Reply 99 is a flood rejection and applies
its penalty; reply 100 remains explicitly ambiguous even for code zero. Existing bounded reconciliation may perform only the
already authorized exact-ext cleanup; no automatic AddOrder resend is introduced. Journal output preserves raw response bytes.

The configurable rolling-second publisher gate retains conservative attempt accounting across host/publisher/environment reopen.
Throttling is DefinitelyNotSent and performs no publisher call. Boundary, clock regression, penalty and reopen tests pass.
Default 30 is a local conservative cap, not the exchange-provisioned rate. Native host configuration accepts 1..3000; process-wide
or cross-process persistence/coordination is not added. [System/rate details](../../plaza2/CERT_SYSTEM_REPLIES_RATE.md).

AGGR20's two prior bridges are replaced by one shared bridge. Stale levels/readiness are invalidated on loss, LifeNum, callback
failure and stop. CLOSED/ERROR recovery waits one second and bootstraps a fresh snapshot. ClearDeleted uses conservative
invalidation plus resnapshot, not selective range compaction. Private mid-run recovery policy remains unchanged.
[AGGR scope and tests](../../plaza2/AGGR20_OFFLINE_RECOVERY.md).

## Validation and raw performance

- Final Release: **171/171 PASS**, including 59 PLAZA-labelled tests and all three preflight checks.
- Actual ASan/UBSan Debug build: **74/74 labelled tests PASS**, plus **3/3 AGGR tests PASS**; no sanitizer findings.
- LeakSanitizer is unsupported on this macOS runtime. The initial detect_leaks=1 invocation aborted before tests; supported
  ASan/UBSan runs used detect_leaks=0. No Linux sanitizer or leak-detection pass is claimed.
- Earlier C1 full regression encountered an unchanged timing-dependent TWIME establish test failure; the final full Release
  and sanitizer runs both passed it. An intermediate test-edit recursion and a wrong callback constant were corrected before
  passing results; neither is hidden by the final success counts.
- Evidence logs and hashes: [validation.json](validation.json), [Release](release_ctest.log),
  [ASan/UBSan](asan_ubsan_ctest.log), [additional AGGR](asan_ubsan_aggr.log).

The D0 harness exercises wire-style input -> actual callback dispatch -> decode -> revision checks -> transaction visibility ->
mandatory and optional bounded output. It processed 9 million records across five profiles, with zero decode/revision failures
and zero mandatory/optional drops. All queues ended empty.

| Profile | Observed msg/s | Duration | Queue high-water |
| --- | ---: | ---: | ---: |
| Offered 100k | 99,977.8 | 10.0022 s | 1,000 |
| Offered 200k | 199,980 | 10.0010 s | 1,000 |
| Unpaced burst | 16,746,000 | 0.119432 s | 32,768 |
| 4,096 instruments | 199,983 | 10.0008 s | 1,000 |
| Million-ID population | 199,979 | 10.0011 s | 1,000 |

Host: Apple M4 Pro, Darwin 24.6.0 arm64, Apple clang 17 Release. Peak cumulative process RSS 35,586,048 bytes; maximum sampled
oldest queue age 6,141,291 ns. Offered-rate profiles allow 2% pacing overhead and retain the measured values, not rounded-up claims.
The million-ID population is generator coverage, not an active L3 map. Binary hashes and all metrics are in [raw_d0.json](raw_d0.json).
These are short local synthetic **raw-only** results. Vendor/network receive cost, Linux target hardware and L3 mutation are absent.
`certification_performance_pass` remains false.

## Remaining work before C2

1. Obtain/reproduce negotiated composite p2ordbook callback schemes and both regular/multileg pairing fixtures.
2. Verify multileg publication coordination, composite restart, LifeNum and retained-history/retention-exhaustion behavior.
3. Freeze public order identity, rollover, action/remainder and full-book mutation invariants with authoritative evidence.
4. Review this raw tranche and resolve those protocol ambiguities before implementing C2.

After C2, repeat performance through the complete L3 path on declared deployment hardware; longer endurance, coexistence and
coordinated exchange failure scenarios remain necessary. The updated [96-row matrix](../../../cert/PLAZA2_CERT_MATRIX.md)
keeps those gaps explicit. No claim of live certification or full L3 readiness is made.
