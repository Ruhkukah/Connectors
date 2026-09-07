# C2 capture harness preparation

Base: PR #43 `9a2974e8921c3ef6904582842c73950b306de726`.
Branch: `codex/plaza2-c2-capture-20260907`; stacked draft, no merge authorized.

Implementation: native listener-only recorder, immutable version-1 binary trace,
actual OPEN descriptors, bounded loss detection, automatic descriptor-driven
import and existing PR #43 oracle replay. Detailed format, API allowlist,
configuration and later authorized exercises are in
[the runbook](../../plaza2/C2_CAPTURE_HARNESS_9_9.md).

Local Release: 6/6 tests passed. Local macOS arm64 ASan/UBSan: 6/6 passed,
`detect_leaks=0` because macOS does not support LeakSanitizer. Linux CI with
`detect_leaks=1` and the deliverable Linux x86_64 artifact are pending.
These local tests cover the implementation tree before this preparation commit;
embedded local binary Git metadata therefore identifies the base revision.
The clean Linux CI build will identify the CI checkout revision.

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
