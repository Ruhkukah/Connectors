# C2-PREFLIGHT closeout — production blocked

- Main base: `22a9dfd0947ccde4645696974e1270099491e2c6`.
- Stack/PR base: `585d199997ec65d4690cc9618350052509c9d83b` (PR #42).
- Implementation and locally tested source head: `e66cadc0f3731d409758090713eeb9c082201fa1`.
- Draft [PR #43](https://github.com/Ruhkukah/Connectors/pull/43), branch `codex/plaza2-c2-preflight-20260907`, based on
  `codex/plaza2-aggr-rec-20260907`. The subsequent closeout commit adds this receipt only; final review HEAD is reported on the PR.
- PRs #38–42 remain unmerged. No T1, publisher calls, exchange orders, production L3 projector, public C ABI or managed API changes.

## Findings and exact gate verdict

**Production C2 remains BLOCKED: the eight required gates are not all PASS_OFFLINE. No certification or full multi-table L3
readiness is claimed.** The conditional model passes; it cannot replace unavailable protocol authority.

| Item | Result |
| --- | --- |
| Regular negotiated mapping | REQUIRES_T1_CAPTURE. No actual OPEN/schema; synthetic permutations validate descriptor-based decoding only. |
| Multileg negotiated mapping | REQUIRES_T1_CAPTURE. Candidate separate pair constructs; neither actual callback topology nor cross-pair atomicity is proven. |
| Identity domain | BLOCKED. No production key frozen. Universal uniqueness/reuse/same-ID rules require written MOEX authority. |
| Mutation semantics | PASS_OFFLINE for official actions 0/1/2, direct remainder, zero-rest execute removal and terminal cancel. Insertion equality/cancel quantity relation not asserted. |
| Snapshot validity | Publication 0/1 and bound fields documented; actual composite info/transaction chronology REQUIRES_T1_CAPTURE. |
| Ready | Current generation/life, complete committed valid snapshot, ONLINE, no incomplete transaction/failure/contradiction, mandatory frontier fully drained. |
| LifeNum | PASS_OFFLINE model invalidation at all six requested positions; all old data, index, pending work, frontier and borrowed handles invalid. |
| ClearDeleted | Four table-specific callback cases. Historical compaction is not cancellation; ambiguous relevant case NeedsResync and fresh bootstrap. Selective policy needs capture. |
| Restart/failure | PASS_OFFLINE conservative model: invalidate immediately, fresh composite bootstrap, no raw-token-only L3 restoration. |
| Retention failure | Explicit fake open ERROR; one-second retry policy, three attempts then Failed. No exchange retention duration or arbitrary-downtime guarantee. |
| Equivalence | PASS_OFFLINE within stated synthetic domain; real protocol gate BLOCKED pending negotiation and identity. |

The SDK probe ran in an amd64 Docker container with `--network none`. Only container loopback port 1 was attempted;
connection-open returned 131073 and the vendor log records connection refused. Both listener constructions returned 0;
getscheme/open returned 131077, no schema and zero callbacks. This is a limited failed negotiation experiment, not proof of a
vendor-simulated composite. No extra FTP file is currently known to be required.

## Files and scope

| Files | Purpose |
| --- | --- |
| `docs/plaza2/PUBLIC_L3_MUTATION_CONTRACT_9_9.md` | Authority, unresolved identity, mutation/recovery rules, all gates, data-structure design |
| `docs/plaza2/C2_MARKET_DATA_CAPTURE_9_9.md` | Unexecuted listener-only capture scenario and written MOEX questions |
| `tests/plaza2_cgate/plaza2_c2_reference.hpp` | Pure conditional reference model; no production linkage |
| `tests/plaza2_cgate/plaza2_c2_preflight_test.cpp` | Descriptor binding, callbacks, explicit faults, independent truth, property/restart oracles |
| `tests/plaza2_cgate/fake_cgate_runtime.cpp`, `fake_cgate_abi.hpp` | Extend existing fake shared library; move its unchanged ABI declarations to shared test header |
| `tests/fixtures/plaza2_c2/sdk_probe.cpp`, `README.md` | Reproducible official SDK experiment and honest fixture provenance |
| `tests/CMakeLists.txt`, `.github/workflows/ci.yml` | Register tests and audited no-publisher selection for this draft only |
| `cert/PLAZA2_CERT_MATRIX.md` | Preserve 96 distinct rows; attach preflight evidence without promoting production capability |
| This report, `validation.json`, four logs | Source-bound evidence and limitations |

The normal test suites remain configured. This PR's branch builds only the four required native targets and selects `c2_preflight` in both CI jobs to honor the literal
no-publisher constraint, including fake publisher calls. Other PRs/main retain full suites. This is a focused regression result,
not a requalification of the entire stack. Existing raw ORDLOG, wire checks and D0 smoke are included because they exercise
shared fake-runtime changes without a publisher.

## CI correction

The initial Linux sanitizer run identified a `const void*` versus `void*` callback function-pointer mismatch in the new test
driver and a missing PyYAML dependency for the selected codegen check. The fake library and consumer now share the exact
callback typedef; the sanitizer job installs the existing requirements for this draft. Local results below were rerun after
the correction. Neither issue affected production code. Final Linux status is attached to the exact PR head.

## Validation

| Check | Result |
| --- | --- |
| Local Release, `ctest -L c2_preflight -V` | 5/5 passed; [log](release_ctest.log) |
| Local ASan/UBSan, same selection | 5/5 passed; [log](asan_ubsan_ctest.log) |
| ASan configuration | Actual address/undefined instrumentation, halt on error; macOS `detect_leaks=0` because LeakSanitizer unsupported |
| Linux CI | PR checks run the same no-publisher selection; Linux uses `detect_leaks=1`. See PR checks for exact revision/result. |
| Explicit scenarios | 6 LifeNum positions, 6 CLOSE/error positions, 4 pair/permutation cases, 18 callback faults, 2 failed opens, snapshot/anomaly/overflow/fatal-latch cases |
| Property histories | 99: 32 seeds × 3 pair modes, plus three 10,000-order histories |
| Snapshot equivalence | 1,136 full-map and canonical-hash equalities |
| Restart equivalence | 1,136 interruptions and fresh-bootstrap equalities |
| Illegal histories | 396 single-element corruptions at seeded offsets fail closed |
| Repository checks | Source formatting, repository style, Unicode guard and staged diff check passed |
| Certification matrix | 96 unique capability rows preserved |

Canonical content includes exchange generation, logical pair, ID, session, instrument, side, integer price, direct remainder
and both status masks. Local borrowed-handle epoch deliberately changes at restart and is validated separately. The mixed
oracle composes independent test domains; no cross-pair atomic publication is inferred. Reference map/digest allocation and
speed are not production performance evidence. The future design uses bounded slots, ID lookup, per-instrument/price-level
indices and generation-tagged borrowed views; it has not been implemented.

Remaining capture items: actual regular/multileg OPEN descriptors and indices; info/control/transaction exposure; complete
snapshot witness and bound continuation; repeated publications/replay; LifeNum/substream relationship; ClearDeleted effects;
actual retention/failure traces and multileg coordination. Identity scope and universal reuse invariants require written MOEX
confirmation in addition to any capture. Stop at this gate; the capture document is not authorization to access T1.
