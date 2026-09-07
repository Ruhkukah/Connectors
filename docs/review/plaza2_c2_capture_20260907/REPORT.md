# C2 capture harness preparation

Base: PR #43 `9a2974e8921c3ef6904582842c73950b306de726`.
Implementation head: `a512a67bf66fdd0bed55bdce68a66f8d4afe9aad`.
Draft [PR #44](https://github.com/Ruhkukah/Connectors/pull/44), branch
`codex/plaza2-c2-capture-20260907`. This closeout commit adds evidence only;
the final review head is reported on the PR. No merge authorized.

Implementation: native listener-only recorder, immutable version-1 binary trace,
actual OPEN descriptors, bounded loss detection, automatic descriptor-driven
import and existing PR #43 oracle replay. Detailed format, API allowlist,
configuration and later authorized exercises are in
[the runbook](../../plaza2/C2_CAPTURE_HARNESS_9_9.md).

Local Release: **6/6 PASS**. Local macOS arm64 ASan/UBSan: **6/6 PASS**,
`detect_leaks=0` because macOS does not support LeakSanitizer. Linux Release:
**6/6 PASS**. Linux ASan/UBSan with **LeakSanitizer detect_leaks=1: 6/6 PASS**.
[CI run 34157871238](https://github.com/Ruhkukah/Connectors/actions/runs/34157871238)
passed both jobs at implementation head a512a67. The existing shared C ABI target
also rebuilt successfully after SHA extraction (build only; no publisher tests).

The first Linux attempt passed sanitizers but failed one clang-format declaration;
that formatting discrepancy was fixed. The same update enforces declaration of
an environment INI for source hashing and credential redaction. Invalid INI
configuration is rejected before loading the runtime or creating output.

Artifacts are retained outside the repository at
`/Users/pavel/CSharp/moex-c2-capture-prepared-20260907/`:
`linux-amd64/build/apps/plaza2_c2_capture` is an ELF64 little-endian x86_64 Release
binary; `macos-arm64/plaza2_c2_capture` is Mach-O arm64. Oracle and importer are
included. SHA-256 values are in `validation.json`. The Linux artifact embeds the
GitHub merge-checkout SHA (recorded in the CI log), whose tree matches the tested
implementation; the macOS deliverable was reconfigured at a512a67. Local test
logs precede their source commit; the retained fake trace is explicitly from
b009248, and its own embedded commit/executable/source hashes remain authoritative.

Eight fake-native runs cover regular/multileg bootstrap and regular reopen,
permuted indices, unknown table and control, transactions/ONLINE, LifeNum,
ClearDeleted/CLOSE/error, exact multileg rejection and diagnostic redaction,
unexpected descriptor, quiet activity, overflow and OS write failure. Corruption
and truncation are rejected; import preserves the original bytes and SHA.
Reopen comparison passes conditionally at common committed frontier 15 in life 7.
Quiet data is NOT_OBSERVED. Every fake API call belongs to the documented
16-function allowlist: publisher creations/opens/posts and transaction commands
are all zero. The binary contains no publisher or ConnectorHost symbols.

The SHA implementation was extracted unchanged into its own static library to
avoid linking the transaction-capable runtime object. The existing fake ABI now
forwards to the shared capture ABI; no connector public ABI was expanded.
CI changes are limited to this capture/preflight branch selection, Release
configuration for that selection and a downloadable Linux artifact.

No T1, real CGate connection, publisher execution, orders, network disruption or
production L3 work occurred. No additional FTP item is currently required.
Regular/multileg mapping and snapshot chronology REQUIRE_T1_CAPTURE;
public_order_id identity remains BLOCKED pending written MOEX authority.
Production C2 is BLOCKED. PRs #38–44 must remain unmerged.

Remaining work after explicit authorization: configure approved market-data-only
runtime/access, perform bounded regular bootstrap/passive capture, optional
regular listener reopen and candidate multileg capture, retain immutable hashes,
run the importer/oracle, and inspect actual mapping/chronology/bounds/generation.
No activity may be manufactured with orders. Capture completion does not promote
any certification gate automatically.

## Changed files and callable API surface

- `apps/plaza2_c2_capture.cpp`, `apps/plaza2_c2_capture_abi.hpp`: recorder and shared native descriptor ABI.
- `tools/plaza2_c2_capture_import.py`: strict immutable trace importer, analysis and oracle fixture generation.
- `tests/plaza2_cgate/plaza2_c2_capture_test.py`: fake-native safety, import and failure tests.
- `tests/plaza2_cgate/fake_cgate_runtime.cpp`, `fake_cgate_abi.hpp`, `plaza2_c2_preflight_test.cpp`: API audit, composite fixtures and existing-oracle import mode.
- `protocols/plaza2_cgate/src/plaza2_sha256.cpp`, `plaza2_runtime.cpp`: unchanged SHA algorithm extracted from transaction-capable runtime object.
- `.github/workflows/ci.yml`, `apps/CMakeLists.txt`, `tests/CMakeLists.txt`, `protocols/plaza2_cgate/CMakeLists.txt`: targets, links, tests and Linux artifact.
- `docs/plaza2/C2_CAPTURE_HARNESS_9_9.md`, `C2_MARKET_DATA_CAPTURE_9_9.md`, this review directory: runbook and evidence.

Exactly these APIs are resolved and were called by the retained fake capture:

```text
cg_env_open cg_env_close cg_env_getcomp_ver cg_err_getstr
cg_conn_new cg_conn_open cg_conn_close cg_conn_destroy
cg_conn_process cg_conn_getstate
cg_lsn_new cg_lsn_open cg_lsn_close cg_lsn_destroy
cg_lsn_getstate cg_lsn_getscheme
```

`fake_receipt.json` records counts for each API: 38 callbacks, 64 persisted frames,
18,166 bytes, no loss; zero publisher creations/opens/posts and transaction commands.
Regular and multileg descriptors negotiated in the fake success case, and the
regular reopen produced one equal common-frontier comparison with zero conflicts
or model rejections. The other fake cases retain rejection/unexpected/quiet outcomes.
Overflow and real OS write failure exit CAPTURE_INVALID without finalized traces.

Trace version 1: `P2CAP001`, little-endian length-prefixed fixed 88-byte headers,
exact payload/null bytes, actual descriptor blocks, control/API records,
prefix-SHA footer and whole-file SHA sidecar. Derived indexes live in `derived/`;
source trace bytes are never changed. This is prepared tooling, not real capture
or proof of actual MOEX negotiation or identity.
