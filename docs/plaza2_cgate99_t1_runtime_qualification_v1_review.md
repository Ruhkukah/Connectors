# PLAZA II CGate 9.9 T1 Runtime Qualification V1

## Scope and safety boundary

This prerequisite qualifies the existing connector against the current official
MOEX T1 CGate 9.9 stack. It does not contain PR #30 pre-send logic, allocate or
post publisher messages, submit an order, change a production profile, or remove
the historical CGate 9.3 evidence.

Authoritative base: `main` at
`610dbfd440bda86ccbc871ca0c755e591a74e363`. PR #30 remains frozen at
`e589eba93a9e4bcd4491ae75fc0a631b5f219426`.

## Pre-edit CGate 9.3 assumption inventory

This inventory was completed before runtime or connector code was changed. The
dispositions below distinguish current-T1 inputs from historical provenance;
they are not a mechanical `93` to `99` replacement.

| Path | Symbol or lock | Existing CGate 9.3 assumption | CGate 9.9 disposition |
| --- | --- | --- | --- |
| `protocols/plaza2_cgate/src/plaza2_runtime.cpp` | CGate ABI structs/constants | ABI comment names 9.3 | Compare both headers; retain only identical layouts. |
| `plaza2_runtime.hpp` | Runtime settings/report | Library identity only reported | Add an exact current-T1 identity lock. |
| `plaza2_test_trade_transport.cpp` | AddOrder state comment | Semantics named 9.3 | Relabel only after 9.9 field comparison. |
| `runtime_scheme/` | Runtime scheme lock | SPECTRA93 hash | Preserve it; add a release-qualified 9.9 lock. |
| `trade/manifest.yaml` | Transaction lock | SPECTRA93 hashes | Preserve it; add a compared 9.9 manifest. |
| `plaza2_runtime_scheme_lock.py` | Consumed scheme check | One vendor scheme | Run unchanged logic against 9.9. |
| `tools/plaza2_phase5a_trade_materialize.py` | hard-coded 9.3 source fingerprints | materializes the historical 9.3 trade spec lock | Keep historical generator stable; do not overwrite its provenance with 9.9 |
| Phase 3A locks/manifests | Documentation inputs | Include production 9.3 evidence | Preserve unchanged. |
| `profiles/test_plaza2_*` | TEST runtime selection | Select SPECTRA93 | Select exact reviewed 9.9; leave production unchanged. |
| PLAZA/TWIME tests | Fixture release markers | Mostly SPECTRA93 | Keep history; add 9.9 and mixed-identity rejection cases. |
| Existing PLAZA review docs | Historical review claims | Describe the qualified 9.3 stack | Preserve as dated evidence; supersede only current T1. |
| `docs/evidence/plaza2_test_t1_qualification_20260825/` | prior live receipt | exact SPECTRA93/CGate 9.3 fingerprint | Preserve unchanged as dated evidence; it is no longer current-T1 authority |

The remaining inventory searches found no committed CGate vendor binary,
credential, authentication file, or raw router configuration to migrate.

## Frozen official 9.9 source material

The exact official stack already proven by the read-only control is:

| Artifact | Version or filename | SHA-256 |
| --- | --- | --- |
| CGate package | `cgate_9.9.0-2008_amd64.deb` | `15047b505bfdb2c73c6ec6c4457ddf05b690a234cf98c41f085d8b07c7e60eb2` |
| CGate distribution | `cgate_linux_amd64-9.9.1853.zip` | `49da634749203a919e69132594198348652acb5e731416c8a39bb34969364ce2` |
| Router | `P2MQRouter-229.123.0.7233` | `6ea6ad50d6e3300fee99e43900eb22a21c207166f2455d2fc850f7e30bc91423` |
| Runtime library | `libcgate.so.6.102.0.6118` | `f63e726a8482b793c3af755a8dc2b9ebb5cd727d88fb58ebb3fe9704a155ce6f` |
| `cgate.h` | CGate `6.102.0` | `b057b537034b23e960f27477dab6738ba5d751ac698287a40d8f95ba7a1ef78f` |

The matching scheme marker is `SPECTRA9.9.0` with DDS version
`990.1.6.42744`. Only hashes and derived descriptions are committed; vendor
binaries, credentials, authentication material, and raw router configuration
remain outside the repository.

## Qualification status

## Compatibility result

- The consumed x86-64 CGate ABI is byte-layout compatible between 6.93.1 and
  6.102.0. The only observed header-version changes are the version macros.
- AddOrder, DelOrder, DelUserOrders, and their consumed reply layouts are
  byte-for-byte compatible with the historical SPECTRA93 lock.
- The SPECTRA 9.9 replication schemes are compatible with six reviewed
  non-consumed legacy-field removals. All other missing or type-changed
  required fields remain fatal.
- USERORDERBOOK and TRADE remain independent TEST evidence surfaces. POS to
  TRADE anchor coherence remains mandatory.
- Runtime acceptance is release-keyed and hash-locked. An unknown or mixed
  library/scheme identity is rejected.
- The deterministic fake runtime now exposes the reviewed raw object-state ABI:
  `CLOSED=0`, `ERROR=1`, `OPENING=2`, and `ACTIVE=3`. The adapter regression
  checks all four values against the committed 9.9 ABI lock.

## Validation

| Gate | Result |
| --- | --- |
| Full CTest | 157/157 passed |
| PLAZA label | 47/47 passed |
| TWIME label | 74/74 passed |
| .NET ABI and no-send selection | 4/4 passed |
| Minimal no-test/no-operator build | passed |
| Linux changed-target ASan/UBSan with leak detection | 7/7 passed |
| Fake-state amendment changed-target ASan/UBSan | 1/1 passed |
| Source/repository style and Unicode guard | passed through full CTest |
| `git diff --check` | passed |
| Credential/privacy scan | no secret or broker endpoint added |
| Publisher API guard | no application `cg_pub_msgnew` or `cg_pub_post` path added |

Apple ASan cannot run with `detect_leaks=1`; the authoritative Linux run used
`detect_leaks=1:halt_on_error=1` and UBSan halt-on-error. The complete macOS
sanitizer label otherwise passed except for an intermittent Apple loader report
inside compiler-generated globals of the repeatedly loaded fake CGate dynamic
library. The same affected transport target passed under Linux ASan/UBSan.
The final fake-state-only amendment passed its changed runtime-adapter target
under Apple ASan/UBSan with leak detection disabled; final-head Linux CI remains
the authoritative leak-enabled sanitizer gate. No additional live T1 run was
required because the amendment changes only the deterministic test runtime.

## Live 9.9 parity

The official read-only control is `OFFICIAL_9_9_REPLICATION_READY`. The
connector then reached ACTIVE/ONLINE and snapshot-complete state on REFDATA,
AGGR20, POS, PART, SESSIONSTATE, INSTRUMENTSTATE, USERORDERBOOK, and the
POS-anchored TRADE replay. USERORDERBOOK periodic consistency was true.
Publisher and its same-name p2mqreply listener opened successfully. Runtime
identity was `6.102.0.6118` / `SPECTRA9.9.0`; fatal scheme drift was zero.

The exact implementation head exercised live was
`e02694f3f7d873eef089da678765d7d36e55a80b`. The redacted receipt is
`docs/evidence/plaza2_cgate99_t1_runtime_qualification_20260901/cgate99_connector_parity_receipt.json`
with SHA-256
`76c21df7eb749d28dda48f82c2622bfa5e1f83c4071388a5008f9c5504e34cce`.
Every live control had `cg_pub_msgnew=0`, `cg_pub_post=0`, and no order.

## PR #30 integration note

Frozen PR #30 contains a pre-merge async-listener supervisor written against
the old fake-state mapping. After PR #31 merges, PR #30 must be amended to use
the reviewed CGate state values before any 9.9 `LiveTestPreSend` run.

## TEST router disposition

The initial work order preserved the restored 9.3 service. The operator later
explicitly superseded that constraint and requested the current 9.9 router as
the active TEST runtime. `cgate99-t1.service` is therefore enabled and active
with the exact reviewed router and library hashes. The legacy `cgate@t1.service`
is disabled and inactive; its files and historical evidence remain available
for rollback/provenance and are not used by the active 9.9 stack.

## Classification

`CGATE99_CONNECTOR_PARITY_PASS`

This prerequisite does not qualify an account position, authorize PR #30, or
authorize an order. PR #30 remains frozen until this prerequisite is reviewed
and merged.
