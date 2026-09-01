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
| `protocols/plaza2_cgate/src/plaza2_runtime.cpp` | handwritten CGate constants and callback structs | comments bind the consumed ABI to the CGate 9.3 header | Compare constants, sizes, alignments, and offsets against both headers; retain layouts only if identical and relabel the reviewed ABI set |
| `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_runtime.hpp` | `Plaza2Settings`, `Plaza2RuntimeProbeReport` | release and scheme hash are checked, but the runtime library version/hash is only reported | Requires an explicit current-T1 runtime identity lock and fail-closed unknown combinations |
| `connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp` | AddOrder state interpretation | comment identifies the verified semantics as 9.3 | Compare the consumed 9.9 transactional fields and values; update the qualification label only if unchanged |
| `spec-lock/test/plaza2/runtime_scheme/` | runtime manifest and derived scheme signature | exact `cgate_9.3.0.1687`, `SPECTRA93`, and 9.3 `forts_scheme.ini` hash | Preserve as historical provenance; add a release-qualified 9.9 current-T1 lock and regenerated derived reports |
| `spec-lock/test/plaza2/trade/manifest.yaml` | transaction and private replication source hashes | exact 9.3 message/scheme hashes and `SPECTRA93` markers | Preserve as historical provenance; add a release-qualified 9.9 manifest after consumed-field comparison |
| `tools/plaza2_runtime_scheme_lock.py` | required consumed replication tables and compatible integer widths | reviewed metadata is compared with one supplied vendor runtime scheme | Logic is version-neutral; run it against 9.9 and commit the derived release-qualified output |
| `tools/plaza2_phase5a_trade_materialize.py` | hard-coded 9.3 source fingerprints | materializes the historical 9.3 trade spec lock | Keep historical generator stable; do not overwrite its provenance with 9.9 |
| `tools/plaza2_phase3a_materialize.py`, `spec-lock/{test,prod}/plaza2/manifest.yaml` | Phase 3A documentation locks | reviewed 9.3 documentation inputs, including production | Historical documentation provenance; unchanged, especially production |
| `profiles/test_plaza2_*` and local examples | `expected_spectra_release: SPECTRA93` | current TEST examples select the obsolete T1 release | Current TEST templates/examples require the reviewed 9.9 release and runtime identity; production profiles remain unchanged |
| `tests/plaza2_cgate/*`, `tests/plaza2_trade/*`, `tests/plaza2_twime_integrated/*` | fixture release markers | most synthetic fixtures identify as `SPECTRA93` | Keep historical compatibility tests where useful; add explicit 9.9 acceptance and unknown/mixed-combination rejection tests; update tests tied to current TEST templates |
| `docs/plaza2_phase4e_runtime_scheme_lock.md`, `docs/plaza2_phase5a_trade_spec_lock.md`, `docs/plaza2_phase5d_aggr20_market_data_bringup.md`, `docs/plaza2_test_connectivity_qualification_v1_review.md`, `docs/plaza2_order_lifecycle_v2_review.md`, `docs/plaza2_test_trade_transport_v1_review.md` | historical review claims | describe the then-installed and reviewed CGate 9.3 stack | Preserve as historical review evidence; this document supersedes 9.3 only for current T1 qualification |
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

## Validation

| Gate | Result |
| --- | --- |
| Full CTest | 157/157 passed |
| PLAZA label | 47/47 passed |
| TWIME label | 74/74 passed |
| .NET ABI and no-send selection | 4/4 passed |
| Minimal no-test/no-operator build | passed |
| Linux changed-target ASan/UBSan with leak detection | 7/7 passed |
| Source/repository style and Unicode guard | passed through full CTest |
| `git diff --check` | passed |
| Credential/privacy scan | no secret or broker endpoint added |
| Publisher API guard | no application `cg_pub_msgnew` or `cg_pub_post` path added |

Apple ASan cannot run with `detect_leaks=1`; the authoritative Linux run used
`detect_leaks=1:halt_on_error=1` and UBSan halt-on-error. The complete macOS
sanitizer label otherwise passed except for an intermittent Apple loader report
inside compiler-generated globals of the repeatedly loaded fake CGate dynamic
library. The same affected transport target passed under Linux ASan/UBSan.

## Live 9.9 parity

The official read-only control is `OFFICIAL_9_9_REPLICATION_READY`. The
connector then reached ACTIVE/ONLINE and snapshot-complete state on REFDATA,
AGGR20, POS, PART, SESSIONSTATE, INSTRUMENTSTATE, USERORDERBOOK, and the
POS-anchored TRADE replay. USERORDERBOOK periodic consistency was true.
Publisher and its same-name p2mqreply listener opened successfully. Runtime
identity was `6.102.0.6118` / `SPECTRA9.9.0`; fatal scheme drift was zero.

The final exact-head receipt and hash are recorded in a follow-up evidence-only
commit. Every live control had `cg_pub_msgnew=0`, `cg_pub_post=0`, and no order.

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
