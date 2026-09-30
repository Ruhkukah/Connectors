# MoexConnector certification readiness register — 2026-09-28

## Verdict and scope

**NOT READY for formal MOEX certification.** Current evidence supports bounded read-only Connector → DTC → Kairos demonstrations only. It does not qualify the intended full Connector
futures/trading product profile, establish exchange-confirmed book authority, grant order authority, or establish formal MOEX acceptance.

The certificate target is MoexConnector as the complete intended selected SPECTRA futures/trading profile. The four-listener read-only DTC demo and optional five-listener DEALS mode are evidence
subprofiles, not a self-declared read-only certificate. Kairos is an external demo UI/client, not the certification target. Full ORDLOG remains optional and out of scope unless separately
selected.

Preserve these current labels:

- `INITIAL_OPEN_SESSION_DATA_READY_AFTER_ONLINE`: **NO** for the bounded midsession initial-open event attempt only; not a finding that all AGGR initialization fails.
- `LISTENER_REOPEN_SESSION_DATA_READY_AFTER_ONLINE`: **NO** for the bounded midsession listener-reopen event attempt only; not a finding that all recovery fails.
- AGGR authority gate: **UNRESOLVED**; these results establish no general synchronization rule.
- The September 28 corroborated DTC snapshots are **provisional display evidence**: LateJoinCorroboratedSnapshot, exchange_confirmed=false, order_entry_allowed=false.
- The long-running observer remains paused. This work did not resume or alter it.
- No order was sent or authorized by this document.

### Status vocabulary

| Label | Meaning |
| --- | --- |
| PASS_OFFLINE (dated) | A deterministic offline test result exists for the stated source/platform. It is not current T1 or formal certification. |
| PARTIAL_OFFLINE | Relevant offline checks pass, but live, platform, scope, or acceptance evidence is still missing. |
| LIVE_PROVISIONAL | A bounded live observation exists, with its candidate, runtime, session, and evidence path stated. It is not a full-day qualification. |
| NOT_RUN_CURRENT | No sufficient execution evidence exists for the current candidate and requirement. |
| IN_PROGRESS_UNVALIDATED | Implementation is present or underway, but the exact current change has not been validated and frozen. |
| MOEX_COORDINATED | Requires an exchange-controlled exercise or provisioned environment. |
| DEFERRED_SCOPE | Deliberately excluded from this certificate profile; source-matrix requirements remain mapped and are not marked passed. |
| USER_INPUT | Legal, commercial, deployment, or product answer owned by the user. |

Test target existence, a green result on a different commit, or a previous order receipt is not current-candidate execution evidence.

## Identity and change control

- **Checkout**
  - **Observed identity / status:** /Users/pavel/CSharp/MoexConnector/.codex-tmp/card07-status-scheme-alias-20260923
- **Branch and tested code checkpoint**
  -  **Observed identity / status:** codex/public-deals-readonly-20260928 at b01ae3f23884425ed6fcdf534a056c50b23c1368; source checkpoint is committed. Base correction:
    00daf9d5909795539f4201c910bbea83028343c0.
- **Kairos source used by the September 28 review**
  - **Observed identity / status:** 2196b79289f7fe1373a0b9e44a87942065ea3d98
- **New Kairos public-deals checkpoint (not deployed)**
  - **Source:** 1cd42a8abeddf706988e336a9b8ccc41ee4a1de8 on `codex/moex-public-deals-20260928`.
  - **Evidence:** DTC-enabled data, chart, study, and backtest suites plus actual C++ fixture interoperability pass below. The app compile check passes; no new GUI binary was deployed.
- **PR #66**
  -  **Observed identity / status:** OPEN, branch codex/live-dtc-runner-20260918, head a8d226241397213e2b21df497cc451da00e76379. GitHub CI checks passed on that PR head; this does not validate
    local 00daf9d or the newer b01ae3f checkpoint.
- **Local September 28 Connector correction**
  - **Observed identity / status:** Source 00daf9d5909795539f4201c910bbea83028343c0. The correction is not pushed to PR #66.
- **Public DEALS work**
  -  **Observed identity / status:** Checkpoint b01ae3f23884425ed6fcdf534a056c50b23c1368. macOS Release 195/195, macOS ASan/UBSan 5/5, Linux Release runner build, and Linux focused 5/5 pass. Live
    DEALS and full Linux certification remain open.
- **Certification manifest**
  -  **Observed identity / status:** cert/aggr_plaza2_certification_manifest_9_9.json still names source_base_sha 78f1dded089453d8e3d52a3f1fc26536baf1b197 and an observation-only binary. It is
    not a manifest for 00daf9d or the b01ae3f DEALS candidate.
- **Questionnaire register**
  -  **Observed identity / status:** docs/review/moex_cgate_questionnaire_register_9_9_20260919.json is DRAFT_NOT_FOR_SUBMISSION, dated 2026-09-19, and records source head
    ffa6552c70bf6b16568ba4f7943c3660019519d2. Refresh its source/profile answers before any submission.

GitHub was checked read-only on 2026-09-28. Relevant public-orderlog work is present as open PRs, not as merged certification evidence:

- **[#39 Raw anonymous ORDLOG pipeline](https://github.com/Ruhkukah/Connectors/pull/39)**
  - **Current GitHub state at check:** OPEN; recorded CI checks green
  - **Head:** b5bd29127555693bd7645967e2be0c96c8218ec0
  - **What it establishes:** A C1 raw ORDLOG pipeline proposal exists. It does not bring this certificate profile into ORDLOG scope or prove MOEX qualification.
- **[#43 C2 preflight contract and fixtures](https://github.com/Ruhkukah/Connectors/pull/43)**
  - **Current GitHub state at check:** OPEN; recorded CI checks green
  - **Head:** 9a2974e8921c3ef6904582842c73950b306de726
  - **What it establishes:** Test-only composite/L3 contract and conditional fixtures. Production C2 is still blocked in the source matrix.
- **[#44 C2 capture harness](https://github.com/Ruhkukah/Connectors/pull/44)**
  - **Current GitHub state at check:** OPEN; recorded CI checks green
  - **Head:** 0008f631b7e4df5f12efade5c8b3d796a1bc8407
  - **What it establishes:** A listener-only capture harness exists; it is not production L3, a live exchange capture, or certification acceptance.

The older T1 full-ORDLOG denial (2026-09-08, REPL:ACCESS_DENIED 40969 / 0xA009) remains historical evidence, not a current entitlement check. Full ORDLOG is optional and out of selected scope.
Keep its rows visible as deferred; do not call its codebase absent.

## Evidence register

### September 28 live evidence

- **E-LIVE-GATE**
  - **Path / SHA-256:** /Users/pavel/CSharp/MoexConnector/.codex-tmp/card07-live-fix-review-20260928/gate/run_identity.json — 0f1577ddf32e05ff7d7593dcd139ae4f18201b813642d69f6995f626228bd45c
  - **Date and result:** 2026-09-28 14:17:08–14:17:10 MSK; helper and analyzer exit 0; order_api_calls=0; publisher_posts=0.
  - **Exact evidential boundary:** A strict read-only, three-stream target/session gate. This is not the four-listener DTC runner receipt or an AGGR reopen test.
- **E-LIVE-TARGET**
  - **Path / SHA-256:** /Users/pavel/CSharp/MoexConnector/.codex-tmp/card07-live-fix-review-20260928/gate/gate_status.json — 07731aa3b058beee0472f13019a943c20c184627e85c7df76d93d27bba8b14bf
  -  **Date and result:** Sampled 2026-09-28 14:17:10.017569+03:00; session 11715, EVENING, ALRS-12.26 / isin_id 4519450, min_step 1.00000, LifeNum 101518061; fresh ONLINE snapshots for REFDATA,
    SESSIONSTATE, INSTRUMENTSTATE.
  - **Exact evidential boundary:** Proves the session/contract target at that gate time. It is not a full-day run, a current five-listener DEALS test, or trading authorization.
- **E-LIVE-ROWS**
  - **Path / SHA-256:** /Users/pavel/CSharp/MoexConnector/.codex-tmp/card07-live-fix-review-20260928/gate/rows.jsonl — a4d7ae0eada83d0f2021c77b05f57438cb94bf66713e3a082d0b757f90a574d4
  - **Date and result:** Immutable copied gate rows; covered by the gate SHA256SUMS.
  - **Exact evidential boundary:** Raw supporting evidence for the three-stream gate. No credential values are included in this review package.
- **E-CONNECTOR-START**
  -  **Path / SHA-256:** /Users/pavel/CSharp/MoexConnector/.codex-tmp/card07-live-fix-review-20260928/connector/runner-startup.jsonl —
    0e16d347eb77280bb175717b376cec94419148858f3d91408a87b76713e781ad
  -  **Date and result:** Review README identifies source 00daf9d…, Linux runner SHA-256 82b8401937506efd450bfaf58a5a819fb5a297a925376d6b8a54f642c89bf34d, runtime library
    f63e726a8482b793c3af755a8dc2b9ebb5cd727d88fb58ebb3fe9704a155ce6f, and scheme 7b93117ee435fd0cb2849b677fc32a9d581364b6ee9afeac9c6c002875400746.
  -  **Exact evidential boundary:** The review says all four strict read-only listeners now use runtime-published server schemes, wait for CGate ACTIVE, and preserve mode=snapshot+online. The
    stream_health object is explicitly pre-warmup; later market-data fields are post-warmup.
- **E-KAIROS-REVIEW**
  - **Path / SHA-256:** /Users/pavel/CSharp/MoexConnector/.codex-tmp/card07-live-fix-review-20260928/README.md — 0f3ff7ea805cede61362ea1b2b72832c40d46f4c0e448f7a5b61e3783d64970e
  -  **Date and result:** September 28 review summary; Kairos GUI source 2196b792…; Release GUI SHA-256 474835e0c394122624a55a136311b71295e93a4105f4f34afc1aca9770aabc4f; live test binary SHA-256
    8dc1e6ee785bd9df96d8ef328749dad196cf08057e72259ab87a4639320dfe31.
  -  **Exact evidential boundary:** Records two bounded serial sessions, each receiving a final DTC message 507 and a committed snapshot: 25 levels in the first, 27 in the later session, with
    different snapshot hashes. Authority is explicitly LateJoinCorroboratedSnapshot, exchange_confirmed=false, order_entry_allowed=false.
- **E-REVIEW-MANIFESTS**
  - **Path / SHA-256:** Review SHA256SUMS — a3a5d0a55669232b4a88201e872dc62d6ad2680f67698dcbcc5d2f94b66be843; gate/SHA256SUMS — d0d760e104efba3713a8fcb45ee0a8f3543f95fd517be81378ab0ceb4553f7f6
  - **Date and result:** Both manifests were checked with shasum -a 256 -c on 2026-09-28; every listed file verified.
  - **Exact evidential boundary:** Confirms copied-file integrity, not the truth of an unrecorded assertion or formal acceptance.
- **E-AGGR-EVENTS**
  - **Path / SHA-256:** docs/plaza2/late_join/evidence.json — 432a17b46d67dd3c304894c21378ec270fd8754e5647b16ef795bc295b356c20
  - **Date and source:** Prepared 2026-09-18 from Connector source 2c5464a96f2014b3d53f3f32015a4d4a7dbc47d5; session 11709.
  - **Result:** Two bounded 120-second midsession attempts; listener opened, snapshot completed, ONLINE, LifeNum 66740, sys_event_count 21. In each, session_data_ready_after_online=false and target_authoritative=false.
  - **Verdict scope:** `INITIAL_OPEN_SESSION_DATA_READY_AFTER_ONLINE` is NO only for the initial-open attempt; `LISTENER_REOPEN_SESSION_DATA_READY_AFTER_ONLINE` is NO only for the reopen
    attempt. This is not a global AGGR initialization or recovery failure.

The live review README and runner receipt support the bounded four-listener demo. The
bundle lacks per-session Kairos GUI captures for both sessions and a full-day certification
bundle. Treat GUI/session details as summary-level until machine-readable and screen artifacts are sealed.

### 2026-09-28 public-DEALS candidate tests

Code checkpoint b01ae3f23884425ed6fcdf534a056c50b23c1368 is committed on
`codex/public-deals-readonly-20260928`, based on correction 00daf9d5909795539f4201c910bbea83028343c0.
The archived source bundle SHA-256 is 4234ba828d9b41cfdd85a186beb3d3d947b13d3c3736ed50db2f0ae9b7339076.
Paths below use `PD`, defined in the source index.

- **E-PD-NEW — `PD/public-deals-new-tests.log`**, SHA-256
  `7711214f05990237b6d3ccd0d5c15d7263d89ff3fe278161e91e96bb6e93c005`
  (2026-09-28 16:13 MSK): 3/3 passed: `connector_host_test`,
  `connector_host_dtc_public_deals_test`, `plaza2_public_deals_test`. Covers the
  five-listener opt-in, DTC 107 producer, and native public-deals offline path.
- **E-PD-WIRE — `PD/public-deals-wire-livetest.log`**, SHA-256
  `64ba991a3a74df1e914fee34ff08b78c75c882d4df24a9b3deca4423e99c7f16`:
  offline loopback LiveTest/wire suite 3/3 passed, including independent generated
  DTC 507 decoding. This is not a MOEX live-session result.
- **E-PD-RUNTIME — `PD/public-deals-runtime-tests.log`**, SHA-256
  `856a0fae969024a709bf0a45644938f3b37fc23323e85f74f322e8989b3a2f3e`:
  3/3 passed: `plaza2_runtime_probe_test`, `plaza2_scheme_drift_test`,
  `plaza2_runtime_listener_test`. Release build, not sanitizer instrumentation.
- **E-PD-HOST — `PD/public-deals-host-tests.log`**, SHA-256
  `60d353a169e8fd0d63bb470153be7e45db676d9880f47ea5cf15c8fbf4f8b13e`;
  **E-PD-HOST-RETRY — `PD/public-deals-host-loopback-tests.log`**, SHA-256
  `9a7d915ce2dbc002dd9b1dd77b577c482ec5e25194e5cc6b06910fd1f2c263ad`.
  Transport, receipt, and host checks are 3/3 across two invocations. The first host
  loopback bind hit sandbox `Operation not permitted`; the permitted isolated retry
  passed 1/1. This was an environment block, not a product failure.
- **E-PD-RELEASE-INITIAL — `PD/public-deals-release-initial.log`**, SHA-256
  `ad54059834d4024c832ccb20d8525967b027020c81dcbb3a74e5009ee3f3d5d8`:
  first macOS Release run 192/193. Sole failure: `source_style_check` while DTC edits
  and an inherited two-line fixture were in progress; formatting was corrected. Independent
  generated .NET 507 decoder and four ABI tests passed. Preserve this initial failure.
- **E-PD-RELEASE-FINAL — `PD/public-deals-release-final.log`**, SHA-256
  `576469d4a6c18b2bab59cdd8ae2ec4c2404c6190074c4ff4a34b772e17915047`
  (2026-09-28 16:23 MSK): full macOS Release CTest 195/195 passed, including both
  new tests, independent .NET 507 decoder, four ABI checks, and `source_style_check`.
- **E-PD-ASAN-FINAL — `PD/public-deals-asan-final.log`**, SHA-256
  `1ab58b0361e2aa3f3ebd3e9707ecb068c13ddef9569583be9f9b27da839b37d3`
  (2026-09-28 16:23 MSK): 5/5 passed: `connector_host_test`,
  `connector_host_dtc_server_test`, `connector_host_dtc_public_deals_test`,
  `plaza2_runtime_listener_test`, `plaza2_public_deals_test`. macOS
  `-fsanitize=address,undefined`, `ASAN_OPTIONS=detect_leaks=0`; not Linux LSan.
- **E-PD-LINUX — executed on the isolated TEST-host build:** checkpoint b01ae3f Release runner
  build PASS and focused Linux tests 5/5 PASS; runner SHA-256
  `0f878644d24b58aa04b658bf9fcafd28be2ec2070bb1c46c2d23b5b871a6276c`.
  Log: `PD/tests-release.log`, SHA-256
  `4ba106710331813fd3a4cce19e632bdddd33b8269a16372584507bff706451f2`.
  Five passing tests: `connector_host_test`, `connector_host_dtc_public_deals_test`,
  `plaza2_public_deals_test`, `plaza2_runtime_listener_test`, `plaza2_scheme_drift_test`.
  Compiler/configure output is in `PD/configure-release.log` and `PD/build-release.log`.
  This is not the full Linux CTest suite or a live vendor-CGate run.
- **E-PD-LINUX-SAN — focused Linux ASan/UBSan/LSan:** the same five targets passed
  from the archived b01ae3f source, with
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
  `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.
  `PD/tests-sanitized-strict.log`, SHA-256
  `688b5a699bcb7c0d404c0e6b574f28b83c2221ecc782fb92e7d712b88aed83fa`.
  Configure/build logs and the first 5/5 sanitizer run are also retained. This is
  focused validation, not the full Linux sanitizer suite or a vendor-CGate soak.
- **E-KAIROS-DATA — DTC-enabled library:** 442 passed, 7 ignored;
  `PD/kairos-data-dtc-final.log`, SHA-256
  `f829d2d6e6f96872c0a6954b64f6d9db4ce0457d7892960bf66b6bb188dd1c2d`.
  Both MOEX and Alor DTC features were enabled. Covers final-FUTURE-507 gating,
  source-session checks, continuity/deduplication, unknown side, retirement,
  repeated AVAILABLE/final-507, and the engine-to-chart event route.
- **E-KAIROS-CPP — actual C++ encoder to independent Rust decoder:** 1/1 passed;
  `PD/kairos-cpp-public-deals-interop.log`, SHA-256
  `bbf9a8b3e0599c0d74cb6629e022d1df37ce6bed5ad4b50600127c78ad94bdb5`.
  Two synthetic localhost trades: IDs 1/2, price 100/101, quantity 1/2, correct
  session/ISIN/replRev/LifeNum/time/xstatus; independent depth epoch 73 and trade
  epoch 1. Depth remained displayable and order entry disabled. These are not MOEX executions.
- **E-KAIROS-CHART — heatmap-enabled chart library:** 249 passed;
  `PD/kairos-chart-final.log`, SHA-256
  `af7d91744e72ced9981c1fd3e50a0572dcfcdc7ec09c56cf12f62f7e39afe175`.
- **E-KAIROS-STUDY — study library:** 478 passed, 5 ignored;
  `PD/kairos-study-final.log`, SHA-256
  `c83428b858d7f63ba050b516a5ef67f0ad32ab4847c24266d99d6cdbff8c1ab6`.
- **E-KAIROS-BACKTEST — simulation library:** 203 passed;
  `PD/kairos-backtest-final.log`, SHA-256
  `afc0ca8c6d214dd8e6505c44b864e19ac9d7df3a58c256ddd39f4cbabbb60f24`.
  An unknown market-data direction cannot become a directional simulated order.
- **E-KAIROS-APP — app compile check:** passed;
  `PD/kairos-app-check.log`, SHA-256
  `91728cdca8dcf72682d47dd6f1e0cf588a990c60110df9c7b62a34340025af31`.
  Formatting and diff checks also pass. The GUI was not launched or replaced.

Kairos library counts are separate feature-specific suites, not a count of unique
certification requirements. Ignored live tests remain unrun; the single explicitly
enabled localhost fixture was run separately and is synthetic.

No live DEALS run or fresh T1 gate/contact occurred this turn: the market is confirmed
closed. The existing observer and service were unchanged. DEALS remains NOT_RUN live.
PR #66's green CI belongs to a8d2262…, not this b01ae3f checkpoint.

### Prior offline evidence and pinned source files

- **E-MATRIX-AGGR**
  - **File / SHA-256:** cert/AGGR_CERT_MATRIX_9_9.md — 6d73069ba7db5d4e3189a3923cbe95140e85d11692132122d517aa7fcc7f069d
  - **Recorded scope and limitation:** 38 AGGR controls: C01–C08, R01–R08, S01–S07, and 15 General rows. PASS_OFFLINE entries remain offline status; T1 statuses are not upgraded by this register.
- **E-MATRIX-PLAZA**
  - **File / SHA-256:** cert/PLAZA2_CERT_MATRIX.md — 9cde1b2a1e226804cb4ed01cba000824f280236a3b0ff780e67389b47a2e1e15
  - **Recorded scope and limitation:** 96 individually identified official/plan rows. It contains wider historical Full ORDLOG/L3 scope than this certificate target.
- **E-MANIFEST**
  - **File / SHA-256:** cert/aggr_plaza2_certification_manifest_9_9.json — ab64fbf69dba3d05d9824126bb25b31bdacec6fdd96649b325cf887720a03908
  -  **Recorded scope and limitation:** Source base is 78f1dded…, verdict AGGR_NOT_READY_FOR_MOEX_CERTIFICATION; live-order lifecycle/full day/restart remain absent there. Not a current candidate
    lock.
- **E-QUESTIONNAIRE**
  -  **File / SHA-256:** docs/review/moex_cgate_questionnaire_draft_9_9_20260919.md — e8f91fb8613deb010d47acb6df28b29654c1134c89b9f1b5cca6974b0b3848e5; register JSON —
    e9a6b170812d7b17e17f2e8d2deb2bf69d9673c99a1385c85dd0dad31e2723bd
  - **Recorded scope and limitation:** Draft register covers 77 fields and 33 printed stream checkboxes. Its source identity and DEALS scope predate this work.
- **E-BASELINE**
  -  **File / SHA-256:** docs/review/plaza2_certification_baseline_20260907.md — eb6f89c6b731fdbc320871c8748509bf51d2dead36fb3fd535ff695bc6873c14; audit JSON —
    d3d2c8d2c1ef9756667cdd482c27b9b289d010271cc6426c3cf5ba90192eadbe
  - **Recorded scope and limitation:** Phase-A audit and architecture baseline; not a current-candidate test result.
- **E-OFFLINE**
  -  **File / SHA-256:** docs/review/plaza2_offline_20260907/REPORT.md — b4c4e76eb20d5aa8d74790fafc07edd95593ad354169b51aa5bd567059d3bb5a; raw_d0.json —
    a9c854ef1ef5f60bc1ffe618b9422eb7a1e94698b59ac0f8981ae539a07b4752
  -  **Recorded scope and limitation:** Historic macOS arm64 raw-path tests/benchmark, not target Linux, not public DEALS, and not full L3. The recorded raw figures include about 99,978 msg/s at
    offered 100k, about 199,980 msg/s at offered 200k, and a short unpaced burst. They do not pass certification_performance_pass.
- **E-C2-LINUX**
  - **File / SHA-256:** docs/review/plaza2_c2_capture_20260907/REPORT.md and linux_release_ctest.log / linux_asan_ubsan_lsan_ctest.log
  - **Recorded scope and limitation:** Historical 6/6 Linux Release and 6/6 Linux ASan/UBSan/LSan for a listener-only C2 capture harness. Not production C2 or the current branch.
- **E-ORDLOG-DENIAL**
  - **File / SHA-256:** docs/review/c2 access record referenced by cert/PLAZA2_CERT_MATRIX.md; trace SHA-256 a05afa4e5de25a8818f8cb91057c5cf02ead8f341636f6880e286e9febf83d66
  -  **Recorded scope and limitation:** 2026-09-08 07:01 MSK: T1 REPL:ACCESS_DENIED 40969 / 0xA009. Preserve as historical entitlement evidence; obtain current entitlement proof if ORDLOG is
    later brought into scope.

## Current read-only demo evidence and acceptance register

The read-only evidence endpoint is not the certificate target. Its default profile has four listeners: AGGR20, REFDATA, SESSIONSTATE, and INSTRUMENTSTATE. It creates no publisher, reply, account,
position, order, AddOrder, or DelOrder surface. Opt-in `--public-deals` adds a fifth anonymous public listener on the same owner; no historical backfill is provided.

- **RO-01**
  -  **Requirement and concrete acceptance:** Establish current session, instrument, and reference-data identity from committed current snapshots. Reject stale, missing, conflicting, partial, or
    wrong-generation metadata.
  - **Tests / command family:** connector_host_test; plaza2_runtime_probe_test; read-only three-stream gate.
  - **Current evidence and next status:** LIVE_PROVISIONAL for session 11715 / ALRS-12.26 on 2026-09-28. Re-discover on every later run.
- **RO-02**
  -  **Requirement and concrete acceptance:** All four default listeners use the reviewed runtime server schemes; wait for CGate ACTIVE before listener creation; open with snapshot+online and
    prove every required listener reaches usable state.
  - **Tests / command family:** connector_host_test; plaza2_runtime_probe_test; plaza2_scheme_drift_test; startup receipt records each listener URL policy/state and scheme fingerprint.
  -  **Current evidence and next status:** The September 28 review records the correction and changing snapshots. Preserve pre-warmup health separately from post-warmup metadata. Freeze and
    qualify the exact current build.
- **RO-03**
  -  **Requirement and concrete acceptance:** Final DTC SecurityDefinition response is truly final and declares FUTURE independently of subscription ID. Test IDs 1, 7, and 101; Replay must not
    make broader claims than LiveTest.
  -  **Tests / command family:** connector_host_dtc_server_test; connector_host_dtc507_generated_schema_test where .NET is available; generated protobuf decoder; LiveTest-mode fake-source socket
    test.
  - **Current evidence and next status:** Review says both serial live clients received final 507. Retain decoded bytes/fields and independent-client receipt on the frozen candidate.
- **RO-04**
  -  **Requirement and concrete acceptance:** Bind DTC instrument metadata to current committed REFDATA identity. Missing board, stale definition, bad currency/value, invalid UTF-8, or
    contradictory source provenance must fail closed; do not fabricate timestamps, board, tick, or instrument identity.
  - **Tests / command family:** connector_host_test; connector_host_dtc_protocol_test; metadata-provenance fixtures; strict UTF-8 tests.
  - **Current evidence and next status:** Current gate proves REFDATA metadata for its target. A full DTC metadata receipt and all adversarial cases still need a sealed current-candidate result.
- **RO-05**
  -  **Requirement and concrete acceptance:** Once definition or display authority changes, reject every eligible unsent stale depth batch; invalidate or close safely; require a fresh final 507
    and complete fresh snapshot before display resumes. Never splice LOGOFF into a partial frame.
  - **Tests / command family:** Metadata-revocation and slow-client/socket tests from Downloads/MOEX_Connector_Audit 2/luna_tasks/03_METADATA_REVOCATION.md; connector_host_dtc_server_test.
  -  **Current evidence and next status:** Prior audit identified this as a contract gate. The new review reports an idempotent repeated-AVAILABLE Kairos fix, but full invalidation/reconnect
    coverage is not in the live evidence bundle.
- **RO-06**
  - **Requirement and concrete acceptance:** A repeated already-true AVAILABLE status is idempotent and does not clear accumulated heatmap history or refresh the definition.
  - **Tests / command family:** Kairos regression repeated_available_status_does_not_revoke_display_or_refresh_definition; active MOEX DTC suite.
  - **Current evidence and next status:** September 28 review says this regression passes. Preserve exact Kairos source/test log with the future frozen package.
- **RO-07**
  -  **Requirement and concrete acceptance:** Read-only startup has no publisher/reply handles or callable order path; order_entry_allowed=false remains visible. Stop/disconnect invalidates the
    data view.
  - **Tests / command family:** connector_host_test; connector_host_dtc_server_test; runtime fake audit for cg_pub_new and reply-listener creation.
  -  **Current evidence and next status:** September 28 review reports no order API calls/posts and the runner is read-only. Re-seal a machine-readable receipt with the exact five- or
    four-listener mode.
- **RO-08**
  -  **Requirement and concrete acceptance:** A live UI is only a demonstration consumer. It must show source status/provisional authority and recover from disconnect by invalidating old data and
    obtaining a new 507/snapshot.
  - **Tests / command family:** Actual Kairos GUI in isolated state; paired Connector/Kairos snapshots; owned client disconnect/reconnect; screenshot plus machine-readable compare.
  -  **Current evidence and next status:** Two live snapshots corroborate changing depth. Kairos is not the certificate target; per-session GUI artifacts and owned disconnect/reconnect proof
    remain outstanding.
- **RO-09**
  -  **Requirement and concrete acceptance:** Preserve the full chain of custody: source SHA, executable SHA, CGate library/header/scheme/config hashes, target/session/instrument, run time,
    receipt hashes, exit codes, and credential redaction.
  - **Tests / command family:** Hash-manifest verification; runner/analyzer exit checks; secret scan; run identity and startup receipt.
  - **Current evidence and next status:** Prior review manifests verified. b01ae3f offline/Linux-focused evidence is recorded below; full five-listener live receipt is absent.

### DTC and authority status

The September 28 three-stream gate and four-listener DTC runner are separate evidence. The gate proves fresh REFDATA, SESSIONSTATE, and INSTRUMENTSTATE snapshots; it does not prove AGGR health or
a DEALS listener. The four-listener runner adds AGGR20.

The review reports two serial DTC/Kairos sessions with final 507 and committed 25- and 27-level snapshots. Different source version, watermark, and hash corroborate changing feed content only.
They do not resolve the AGGR event gate, prove reopen recovery, qualify a full day, or authorize orders. Preserve the two exact NO event labels above.

## Public DEALS work register

The source scheme contains FORTS_DEALS_REPL tables `deal`, `multileg_deal`, `heartbeat`, and `sys_events`; table presence is not proof of a consumer. This change adds an opt-in fifth listener
with a fresh server-published scheme and snapshot+online. Code checkpoint b01ae3f is committed; the results below are offline validation only.

The first-slice contract admits ordinary `deal` rows for the selected session/instrument, `replAct=0`, `nosystem=0`, only after `TN_COMMIT`; bootstrap rows are not replayed as new prints. DTC 101
gates subscribe/unsubscribe and DTC 107 carries subsequent prints behind existing final-507/source-definition gates. Subscription starts at the current tail; there is no historical backfill.
Quiet-market silence is not delivery evidence. See `docs/plaza2_public_deals_dtc.md`.

The opted-in fifth listener participates in strict shared-host recovery: its loss
can interrupt depth, and a malformed callback fails the host closed. Independent
degraded depth service after a DEALS failure is not implemented; the default
four-listener profile is unchanged. The first trade subscription also copies up
to 8,192 retained rows to obtain its tail; a metadata-only lookup is a nonblocking
performance follow-up. Steady-state publication uses a cursor.

Unknown aggressors remain visible as neutral heatmap prints, with their quantities
retained. Existing buy/sell-only candle/profile/ladder/directional-study totals do
not provide a third unknown-volume bucket; do not equate attributed totals with
all public volume. No historical trade backfill is implemented.

- **DEALS-01**
  -  **Acceptance requirement:** With no flag, READ_ONLY_DTC_PROFILE remains exactly four listeners. With --read-only-market-data --public-deals, it is exactly five; the fifth is FORTS_DEALS_REPL
    and is optional.
  -  **Required tests and evidence:** connector_host_test: default config, flag parsing, exact listener count and service list. Inspect fake CGate audit for five cg_lsn_new calls only in the
    opted-in run.
  - **State:** PARTIAL_OFFLINE; full Release and focused suites pass at b01ae3f. Full profile receipt and live run remain open.
- **DEALS-02**
  -  **Acceptance requirement:** --public-deals without --read-only-market-data is rejected before native initialization. The option never selects a publisher, p2mqreply, private TRADE, private
    order, account, position, Add, or Cancel surface.
  -  **Required tests and evidence:** connector_host_test parser/config negative cases; connector_host_test and plaza2_trade_transport_scenarios_test fake API audit; assert zero publisher/reply
    creations and no posts.
  - **State:** PARTIAL_OFFLINE; full Release and focused suites pass at b01ae3f. Live run remains open.
- **DEALS-03**
  -  **Acceptance requirement:** The listener uses only the server-scheme URL p2repl://FORTS_DEALS_REPL with mode=snapshot+online; it shares the ConnectorHost CGate owner thread and has an
    independent readiness/error state.
  - **Required tests and evidence:** connector_host_test and plaza2_runtime_listener_test; fake runtime scheme negotiation; live listener-open receipt and owner map pending.
  - **State:** PARTIAL_OFFLINE; Release/focused tests pass. Vendor-CGate live receipt remains open.
- **DEALS-04**
  -  **Acceptance requirement:** Bind every event to the current committed instrument/session identity; reject wrong or missing isin_id/sess_id and prevent cross-target publication. Only
    committed callback transactions become visible.
  -  **Required tests and evidence:** Fake callback cases for target match/mismatch, incomplete transaction, commit boundary, and ONLINE readiness; current three-stream metadata gate is input
    evidence, not a substitute.
  - **State:** PARTIAL_OFFLINE; tests pass on b01ae3f. Five-listener live identity receipt remains open.
- **DEALS-05**
  -  **Acceptance requirement:** Preserve deal ID, replication identity/revision/action, quantity, session, instrument, both public order IDs, price and status fields without inventing private
    identity or an absent field. Define update/delete and replay behavior, deduplicate only under the documented identity contract, and retain a generation/sequence boundary.
  - **Required tests and evidence:** plaza2_public_deals_test covers duplicate/revision/replay/deletion/LifeNum/ClearDeleted; connector_host_dtc_public_deals_test covers client publication.
  - **State:** PARTIAL_OFFLINE; targeted and full Release suites pass. Full vendor schema/session semantics remain open.
- **DEALS-06**
  -  **Acceptance requirement:** Preserve native `d16.5` as exact fixed-point. DTC 107 Price is mandatory `double`; claim only validated bounded price/tick equivalence after wire decode and
    Kairos price-grid quantization. Do not promise arbitrary-decimal losslessness in DTC.
  -  **Required tests and evidence:** Test native coefficient/scale, sign, null, bounds, finite/range checks, allowed tick equivalence, and Kairos quantization. A future lossless source-decimal
    need requires an explicit raw-value/scale extension.
  - **State:** PARTIAL_OFFLINE; exact arbitrary-decimal DTC precision is not claimed.
- **DEALS-07**
  -  **Acceptance requirement:** Keep `moment_ns` and runtime-decoded `moment` calendar counter distinct. MOEX defines `moment_ns` as UTC Unix nanoseconds. Runtime `moment` is not raw P2TIME
    bytes; its timezone meaning remains unconfirmed.
  - **Required tests and evidence:** Test `moment_ns` unsigned bounds and exact round-trip; retain only the decoded calendar counter for `moment`. Track P2TIME clarification separately.
  - **State:** PARTIAL_OFFLINE; P2TIME timezone unresolved.
- **DEALS-08**
  -  **Acceptance requirement:** Retain unknown market-side/direction/action values as raw or explicit UNKNOWN; never guess Buy/Sell or drop unfamiliar codes. Do not infer aggressor side from
    order-ID magnitude or book changes.
  - **Required tests and evidence:** Use documented MOEX side flags; test absent/contradictory flags, unknown codes, and exact raw preservation in native and DTC views.
  - **State:** PARTIAL_OFFLINE; live semantics remain unqualified.
- **DEALS-09**
  -  **Acceptance requirement:** Loss, malformed required field, incompatible schema, invalid transaction, LifeNum change, callback error, stop, or reconnect must not expose stale trades as
    current; apply the generation/bootstrap gate. ClearDeleted is maintenance: ignore it as a trade, never fabricate a print, and do not invalidate all committed trades solely for this marker.
  - **Required tests and evidence:** Test failure/generation cases and a ClearDeleted fixture proving no synthetic trade or blanket queue invalidation.
  - **State:** PARTIAL_OFFLINE; distinct from AGGR book invalidation.
- **DEALS-10**
  -  **Acceptance requirement:** Bound retained trade history and prove overflow behavior. Do not silently call a truncated ring complete; expose the first lost sequence or mark the view
    incomplete and fail readiness according to the product contract.
  - **Required tests and evidence:** Capacity-boundary, wraparound, duplicate, consumer-lag, and overflow tests; report rate, queue/ring high-water and loss counts.
  - **State:** PARTIAL_OFFLINE; tested bounds are not target-Linux capacity qualification.
- **DEALS-11**
  -  **Acceptance requirement:** Scope claims to the implemented table contract. The reviewed scheme includes multileg_deal, heartbeat, and sys_events as well as deal. Do not imply those tables
    are consumed or qualified unless they have their own decode, transaction, status, and recovery tests.
  - **Required tests and evidence:** Per-table scheme/layout inventory and callback evidence; explicit unsupported/deferred status where no consumer exists.
  - **State:** NOT RUN CURRENT.
- **DEALS-12**
  -  **Acceptance requirement:** Prove actual exposure: a ConnectorHost snapshot is not DTC/client evidence. DTC 101 subscribe/unsubscribe and 107 trade delivery remain behind validated final
    507/source gates; no historical backfill.
  - **Required tests and evidence:** Decode 101/107 on loopback, then match independent live prints by identity, price, quantity, and time.
  - **State:** Offline producer and actual C++→Kairos fixture pass (E-KAIROS-CPP); live DEALS delivery remains NOT_RUN_CURRENT.
- **DEALS-13**
  -  **Acceptance requirement:** Measure the opted-in five-listener mode on target Linux with vendor CGate under representative AGGR plus DEALS traffic. Record loss, queue depth/age, CPU, RSS,
    latency, reconnect, and effect on the existing four-stream mode.
  - **Required tests and evidence:** Linux Release benchmark and overload/recovery harness; paired four/five-listener comparison. Do not substitute the macOS raw ORDLOG benchmark.
  - **State:** NOT RUN CURRENT.

Primary test locations: `tests/connector_host_test.cpp`,
`tests/plaza2_cgate/plaza2_public_deals_test.cpp`,
`tests/connector_host_dtc_public_deals_test.cpp`,
`tests/plaza2_cgate/plaza2_runtime_listener_test.cpp`, and
`tests/plaza2_cgate/fake_cgate_runtime.cpp`.

Native test groups cover snapshot/atomic commit, aggressor flags,
LifeNum/revision/cursor ring, malformed transactions/prices, conflicting identity,
and pending-buffer bounds. After a code change, rerun these and the client suites:

    ctest --test-dir build --output-on-failure -R "^(connector_host_test|connector_host_dtc_public_deals_test|plaza2_public_deals_test|plaza2_runtime_listener_test)$"

Follow with full Release and Linux sanitizer suites; the current focused passes do not close those gates.

## AGGR certification controls

AGGR stable IDs map one-to-one to Appendix 1 Connection 1–8, Replication 1–8, and Sending 1–7; the source IDs are internal traceability IDs. The 15 General rows are additional controls. The
source matrix’s PASS_OFFLINE statuses are not reissued as current T1 or formal passes here.

### Connection C01–C08

- **C01**
  -  **Concrete acceptance / test:** Record exact authenticated T1 connection URL and redacted config identity; test URL construction and invalid/missing settings using
    plaza2_runtime_adapter_test.
  - **Current readiness:** Current URL receipt not packaged for the frozen certificate candidate; NOT_RUN_CURRENT.
- **C02**
  -  **Concrete acceptance / test:** One owner thread controls CGate connection/listeners and callbacks; retain owner map and runtime assertion. Test plaza2_live_session_runner_test and
    connector_host_test.
  - **Current readiness:** Offline design/test evidence exists in the matrix; current full owner receipt pending.
- **C03**
  -  **Concrete acceptance / test:** Keep authenticated router connected for at least 300 seconds with no listeners/publisher; record polling cadence and errors. Run idle-only timer against the
    exact candidate.
  - **Current readiness:** Historical PASS_T1 belongs to c4a0e392e0ed05eec7e264e3fc3cced6def22730 and must be rerun after corrections.
- **C04**
  -  **Concrete acceptance / test:** Prove connection to the authenticated router and record ACTIVE state, source/runtime identity, and redacted endpoint receipt. Use plaza2_runtime_probe_test
    offline plus live receipt.
  - **Current readiness:** Offline support exists; current candidate authenticated-router evidence NOT_RUN_CURRENT.
- **C05**
  -  **Concrete acceptance / test:** Router remains available while upstream Plaza is unavailable; readiness false, no current stream, no send, operator-cancellable wait, no resend. Test
    fake-clock cases then a safe owned upstream fault.
  - **Current readiness:** Offline recovery evidence reported; current live equivalent NOT_RUN_CURRENT.
- **C06**
  -  **Concrete acceptance / test:** Restore upstream on the same connector without app restart; fresh transport generation/bootstrap restores readiness only after all required streams are
    complete and ONLINE.
  - **Current readiness:** Offline recovery evidence reported; current live transition NOT_RUN_CURRENT.
- **C07**
  -  **Concrete acceptance / test:** During established connection, upstream loss immediately removes effective readiness and stale stream use; no automatic Add/Cancel/flatten. Recover only via
    fresh bootstrap and reconcile unresolved epochs.
  - **Current readiness:** Offline fail-closed cases reported; live fault/recovery NOT_RUN_CURRENT.
- **C08**
  - **Concrete acceptance / test:** Local router loss suspends activity and recovers after router restart without blind Add retransmission. Preserve failure cause and attempt history.
  - **Current readiness:** Historical receipts are not relabelled; current-candidate local-router recovery NOT_RUN_CURRENT.

### Replication R01–R08

- **R01**
  -  **Concrete acceptance / test:** Capture every configured replication URL/open argument and negotiated scheme for all five private, two status, AGGR, and any optional DEALS listener in the
    selected profile.
  - **Current readiness:** Current three-stream gate is not the complete listener inventory; receipt needs exact selected profile.
- **R02**
  - **Concrete acceptance / test:** Prove one owner/callback thread per CGate connection and each listener’s ownership; no cross-thread CGate object calls.
  - **Current readiness:** Offline owner tests exist; full current listener map NOT_RUN_CURRENT.
- **R03**
  - **Concrete acceptance / test:** Validate exact required client schemes where explicit and runtime server schemes where URLs are bare. Pin library/header/scheme and negotiated receipt.
  - **Current readiness:** September 28 read-only runner uses server schemes on all four listeners; exact frozen certificate receipt still pending.
- **R04**
  - **Concrete acceptance / test:** Compatible server additions produce explicit warnings while known layouts remain correct; exercise actual negotiated status-stream schemas.
  - **Current readiness:** plaza2_scheme_drift_test and September 28 Linux focused test are cited; retain output and exact runtime fingerprint.
- **R05**
  - **Concrete acceptance / test:** Removed or type-changed required field/table fails closed with named stream/table/field and no readiness.
  - **Current readiness:** plaza2_scheme_drift_test coverage is cited; preserve exact current candidate log.
- **R06**
  - **Concrete acceptance / test:** Interrupt and reopen every declared stream; verify fresh snapshots, ONLINE, source generation, and readiness only after all required streams recover.
  -  **Current readiness:** Current four-listener data receipt is partial; no complete per-stream loss/reopen campaign. The bounded INITIAL_OPEN_SESSION_DATA_READY_AFTER_ONLINE and
    LISTENER_REOPEN_SESSION_DATA_READY_AFTER_ONLINE attempts are each NO; broader recovery is unresolved.
- **R07**
  - **Concrete acceptance / test:** Full ORDERS_LOG ≥100,000 messages/s.
  - **Current readiness:** DEFERRED_SCOPE for the selected certificate profile unless scope is expanded. P2/PF raw-path numbers are not a full ORDERS_LOG or Linux qualification.
- **R08**
  - **Concrete acceptance / test:** Preserve and test LifeNum generation changes and ClearDeleted semantics; invalidate staged/visible state and rebootstrap before readiness.
  - **Current readiness:** Offline AGGR invalidation/fresh-snapshot fallback exists in historical test reports; current T1 transitions NOT_OBSERVED.

### Sending S01–S07

These rows apply only to a later separately authorized trading profile. They are not exercised by the read-only demo.

- **S01**
  - **Concrete acceptance / test:** Capture publisher and reply URLs and exact scheme/profile; prove opens and ownership on the frozen TEST candidate.
  - **Current readiness:** No current-candidate send receipt; NOT_RUN_CURRENT.
- **S02**
  - **Concrete acceptance / test:** Publisher/reply and all CGate objects share the declared owner thread; assert under combined listener load.
  - **Current readiness:** Offline evidence is scoped; current live owner map pending.
- **S03**
  - **Concrete acceptance / test:** Apply configured 1–3000 local rate cap, correct rolling-window boundaries, and retain attempt accounting across reconnect.
  - **Current readiness:** plaza2_publisher_rate_test is an offline test target; provisioned limit and live rate test pending.
- **S04**
  - **Concrete acceptance / test:** Validate exact command send scheme and every declared message layout before any command.
  - **Current readiness:** Codec/spec-lock tests exist; no current-candidate send receipt.
- **S05**
  - **Concrete acceptance / test:** Timeout/unknown send outcome never creates an undefined state or automatic duplicate. Correlate business and private-state channels independently.
  - **Current readiness:** Offline lifecycle/recovery tests cited; current T1 timeout evidence absent.
- **S06**
  - **Concrete acceptance / test:** Decode replies 99/100 as system replies, never as normal success; retain raw response, correlation, and penalty/ambiguity result.
  - **Current readiness:** Offline reply tests cited; no deliberate live 99/100 event. Never flood a shared environment to provoke one.
- **S07**
  - **Concrete acceptance / test:** On publisher/reply loss, block sends, recover handles without retransmitting an ambiguous Add, and reconcile.
  - **Current readiness:** Offline no-post/no-resend tests cited; no current T1 publisher-loss receipt.

### General requirements (15)

- **General — complete interaction logs**
  - **Acceptance requirement:** Complete indexed input, output, result, failure, and system-message log with immutable hash index; no silent gaps.
  - **Current readiness:** Offline journal tests cited; no current full-day indexed log.
- **General — network interruption recovery**
  - **Acceptance requirement:** Controlled safe outage, operator-cancellable recovery, bounded retry cadence, readiness masking, no resend/auto-cancel, and final reconciliation.
  - **Current readiness:** Offline cases exist; current live outage receipt absent.
- **General — application restart during trading**
  - **Acceptance requirement:** Demonstrate zero-order restart and separately one-Working-order restart; restore/reconcile state before readiness and never infer flat from missing state.
  - **Current readiness:** Offline checkpoint tests cited; current T1 restart evidence absent.
- **General — TCS restart with reload**
  - **Acceptance requirement:** Exchange-controlled reload; verify generation invalidation, fresh replay, and complete recovery.
  - **Current readiness:** MOEX_COORDINATED.
- **General — TCS restart without reload**
  - **Acceptance requirement:** Exchange-controlled no-reload restart; verify retained-history/reopen semantics.
  - **Current readiness:** MOEX_COORDINATED.
- **General — reserve/access-server switching**
  - **Acceptance requirement:** Use a MOEX-provisioned alternate access server and record endpoint/config identity, recovery, and state consistency.
  - **Current readiness:** MOEX_COORDINATED.
- **General — full SPECTRA trading day**
  - **Acceptance requirement:** Exercise the declared product through required session transitions, clearing/evening, declared commands, and reconciliation.
  - **Current readiness:** NOT_RUN_CURRENT; fragmented diagnostics and read-only observation are not a full day.
- **General — administrator/emergency procedure**
  - **Acceptance requirement:** Freeze an operator runbook for ambiguity, order/position surprise, router/app/TCS failure; capture acknowledgment and actions.
  - **Current readiness:** docs/plaza2/AGGR_OPERATOR_EMERGENCY_PROCEDURE_9_9.md exists; current-session acknowledgment pending.
- **General — per-instance software identifier**
  - **Acceptance requirement:** Nonempty unique app_name per connection; publisher/reply identity matches; record it in receipt without credentials.
  - **Current readiness:** Offline generation/uniqueness tests cited; exact current-candidate live receipt pending.
- **General — log/system-time ±1 second**
  - **Acceptance requirement:** Capture sync source/status, wall offset, monotonic clock ID, and paired local/exchange timestamps; every pair within one second.
  - **Current readiness:** plaza2_clock_evidence_passes offline; current T1 clock gate NOT_RUN_CURRENT.
- **General — Exchange/NCC messages**
  - **Acceptance requirement:** Retain committed FORTS_REFDATA_REPL.sys_messages with source identity, timestamp, raw text/body, and hash.
  - **Current readiness:** Projector/CLI tests cited; no current-session message receipt.
- **General — broker-system administration/monitoring**
  - **Acceptance requirement:** Resolve applicability against final product and user’s broker/client role; do not infer N/A.
  - **Current readiness:** Questionnaire status remains unresolved; USER_INPUT / NOT_RUN_CURRENT.
- **General — one-to-one MOEX terminology**
  - **Acceptance requirement:** Use current MOEX/VPTS terms in UI, logs, questionnaire, and evidence; preserve source names and reason strings.
  - **Current readiness:** Offline review cited; refresh against frozen candidate.
- **General — fixed SPECTRA subsystem routing**
  - **Acceptance requirement:** Show fixed FORTS/SPECTRA routing and no uncontrolled subsystem selector.
  - **Current readiness:** Source manifest marks fixed profile; preserve in final configuration review.
- **General — broker-system/client-operation applicability**
  - **Acceptance requirement:** Resolve whether the software is proprietary connector or a broker platform offered to clients.
  - **Current readiness:** USER_INPUT where product/legal facts are not settled; do not mark N/A by assumption.

## PLAZA II matrix closure map — all 96 rows retained

The source matrix contains 36 numbered official rows plus 60 PLAN rows. The following map names every source ID. Each retains the source matrix’s independent applicability and status distinction.
Full ORDLOG/L3 is optional and deferred from the selected profile; that exclusion is not a claim that its implementation or qualification is absent.

### Official connection, replication, sending, and general rows (36)

| ID | Original requirement | Current acceptance / status |
| --- | --- | --- |
| P2-C01 | Connection URLs | Exact current inquiry URL and redacted runtime receipt; offline RT tests do not replace it. |
| P2-C02 | Connection thread ownership | Single-owner assertion and current thread/connection receipt; test ownership under combined listeners. |
| P2-C03 | Idle polling ≥5 minutes | Dedicated 300-second idle-only run; active-stream duration is not a substitute. |
| P2-C04 | Authenticated router | Re-prove on current frozen candidate; old T99 bring-up is historical. |
| P2-C05 | Router up, exchange disconnected | No-send wait with explicit recovery state and no current stream. |
| P2-C06 | Detect exchange becoming available | Same running client observes upstream recovery and fresh bootstrap. |
| P2-C07 | Exchange-network loss suspends activity | Mid-run loss, immediate readiness suppression, no stale use, safe fresh recovery. |
| P2-C08 | Router loss suspends activity | Owned router interruption/restart with no blind resend. |
| P2-R01 | Subscription URLs/opening | Record all selected stream URLs, snapshot+online policy, and negotiated schemes. |
| P2-R02 | Subscription thread rules | One owner/callback thread and a complete listener map. |
| P2-R03 | Valid receive schemes | Exact current full-wire scheme check; P2 source records offline PASS for wire, not current live negotiation. |
| P2-R04 | Compatible scheme evolution | Additive changes warn and preserve known fields; retain negotiated unknowns as policy requires. |
| P2-R05 | Incompatible scheme diagnostics | Missing/type-changed required fields fail closed with precise diagnostics. |
| P2-R06 | Every listener reopens | All declared listeners reopen with fresh generation and coherent readiness; current complete outage bundle absent. |
| P2-R07 | Full ORDLOG ≥100,000 messages/s | DEFERRED_SCOPE. Source row remains visible; historical raw-only figures do not satisfy it. |
| P2-R08A | ClearDeleted table/range semantics | Offline AGGR uses conservative invalidate-and-resnapshot; current live ClearDeleted exercise not observed. |
| P2-R08B | LifeNum generation semantics | Offline invalidation tests exist; current exchange LifeNum transition not run. |
| P2-S01 | Publisher URLs/opening | Later trading scope only; exact current TEST URL receipt required. |
| P2-S02 | Publisher thread rules | Same-owner evidence under combined listener load; NOT_RUN_CURRENT. |
| P2-S03 | Configurable command rate gate | Offline plaza2_publisher_rate_test; live cap must match provisioned login and is not established by default 30. |
| P2-S04 | Valid send scheme | Offline CODEC tests; exact current candidate send evidence absent. |
| P2-S05 | Replies/timeouts terminate safely | T1 timeout/unknown-outcome lifecycle and reconciliation remain outstanding. |
| P2-S06 | Reply messages 99/100 | Offline decoding only; live system/flood scenario not run. Never treat as normal success. |
| P2-S07 | Publisher error detection/reopen | No current loss/reopen receipt; no resend after ambiguous send. |
| P2-G01 | Operation/result audit logs | Full declared-command and full-day indexed logs not available. |
| P2-G02 | Network interruption recovery | Offline partial fail-closed tests; current live recovery receipt absent. |
| P2-G03 | Application restart during session | Offline order-host tests do not prove public L3 restore; current-session restart bundle absent. |
| P2-G04 | TCS restart with reload | MOEX_COORDINATED; software generation recovery must precede external proof. |
| P2-G05 | TCS restart without reload | MOEX_COORDINATED; retained-history replay and external evidence required. |
| P2-G06 | Reserve/backup access-server switch | MOEX_COORDINATED; no current alternate endpoint receipt. |
| P2-G07 | Broker administration/monitoring | Applicability unresolved; user/product input required, not assumed N/A. |
| P2-G08 | MOEX terminology mapping | Freeze exact declared tables/commands and use one-to-one terms. |
| P2-G09 | Selected subsystem routing | Freeze SPECTRA target/account/identity and prove routing; historic single target is not a current full run. |
| P2-G10 | Router logging configuration | Redacted config fingerprint and default logging receipt required. |
| P2-G11 | Full trading day including clearing/evening | Full-day evidence absent; coordinate permitted environment and retain complete transitions. |
| P2-G12 | Inquiry/application and test agreement | User-authorized submission and agreed MOEX schedule required; no submission/outreach made here. |

### Additional PLAN rows (60)

| ID | Original requirement | Scope, acceptance, and evidence status |
| --- | --- | --- |
| P2-RT01 | CGate environment initialization | Offline runtime probe/adapter tests and T99 historical evidence; re-lock exact candidate/runtime. |
| P2-RT02 | Router process restart | Offline wrapper coverage is partial; controlled owned restart and fresh state remain NOT_RUN_CURRENT. |
| P2-RT03 | Access-server interruption | MOEX_COORDINATED; retain exact failure/recovery receipt. |
| P2-RP01 | Listener error/close status | Test each declared public listener and preserve exact failure reason; current all-listener run absent. |
| P2-RP02 | Online transition | Existing stream-specific offline/live history is bounded; ONLINE alone does not imply a complete or authoritative book. |
| P2-RP03 | Replstate recovery | Open ORDLOG C1 work exists; durable public state/token pairing and current certification proof remain separate. |
| P2-RP04 | Durable checkpoint after mandatory apply | Atomic state/checkpoint or explicit fresh-bootstrap contract; production public state store not proved. |
| P2-TX01 | No blind retransmission after ambiguous send | Preserve PossiblySent/unknown state, no automatic retry; offline evidence and historical T1 are not current qualification. |
| P2-TX02 | Command-specific reply correlation | Correlate command family, identity, and business reply; do not promote unrelated/duplicate reply. |
| P2-TX03 | Add → Working → Cancel → Cancelled | Later authorized TEST lifecycle only; historical TORDER does not qualify the current source. |
| P2-TX04 | Persistent serial/epoch and restart reconciliation | Reconcile private state across restart; order journal is not a public L3 checkpoint. |
| P2-TX05 | Rate boundary/reconnect/timeout | Offline rate-gate boundaries exist; provisioned limit and current T1 behavior NOT_RUN_CURRENT. |
| P2-MD01 | AGGR20 initial synchronization | Initial-open event attempt NO (E-AGGR-EVENTS); broader initialization qualification unresolved, not globally failed. |
| P2-MD02 | AGGR20 online operation | Historical T99/TORDER evidence is scoped; current DTC corroboration is provisional and not certification. |
| P2-MD03 | AGGR20 recovery | Reopen event attempt NO (E-AGGR-EVENTS); network/TCS recovery remains unqualified, not globally failed. |
| P2-MD04 | AGGR20 LifeNum | Offline invalidation exists; current live generation-change evidence NOT_OBSERVED. |
| P2-MD05 | AGGR20 ClearDeleted | Offline whole-book invalidate-and-resnapshot fallback; current live event/recovery NOT_OBSERVED. |
| P2-OL01 | FORTS_ORDLOG_REPL listener | Optional deferred scope. C1 PR #39 exists OPEN; historical T1 denial remains; no current entitlement/certification claim. |
| P2-OL02 | Exact ORDLOG scheme | Public schema/wire source and C1 work exist; frozen runtime qualification belongs to the optional ORDLOG phase. |
| P2-OL03 | Lossless ORDLOG decoding | Require exact raw payload/null map/BCD/timestamps/flags; do not treat source schema as end-to-end qualification. |
| P2-OL04 | Revision semantics/applied checkpoints | Require transaction commit and mandatory acknowledgment; production persistence equality remains unproved. |
| P2-OL05 | Raw replay/full event output | Require bounded loss accounting and complete stream/generation envelope; C1 PR is open, not accepted. |
| P2-OL06 | Duplicate handling | Distinguish consecutive duplicate, historical replay, and conflicting online revision; retain exact evidence. |
| P2-OL07 | Gap handling | Prove numeric discontinuity vs loss/corruption, mandatory overflow and checkpoint revocation on target runtime. |
| P2-OL08 | ORDLOG LifeNum | Preserve ordered generation context and invalidate active derived state; current exchange test absent. |
| P2-OL09 | ORDLOG ClearDeleted | Preserve raw marker and affected-table semantics; L3 expiry is not implied. |
| P2-OL10 | ORDLOG restart recovery | Distinguish new-process fresh history from same-process continuation; no durable state restoration claim. |
| P2-OL11 | ORDLOG TCS no-reload recovery | C2 replay/retained-history hash equality and coordinated T1 exercise remain. |
| P2-OL12 | ORDLOG TCS reload recovery | C2 new-generation/fresh bootstrap and coordinated T1 exercise remain. |
| P2-L301 | ORDBOOK snapshot | Optional L3 scope; C2 conditional model is not production ORDBOOK. |
| P2-L302 | Bootstrap | Seed active remainder without interpreting snapshot rows as executed orders. |
| P2-L303 | Snapshot revision/LifeNum | Bind basis revision and generation to matching log/publication. |
| P2-L304 | ORDLOG handoff | Verify actual composite handoff callbacks against the frozen scheme; C2 PR remains open. |
| P2-L305 | No snapshot/log race gap | Prove retained incremental coverage across the snapshot boundary. |
| P2-L306 | Deterministic final book/hash | Canonical complete active-state hash, not BBO-only comparison. |
| P2-L307 | Late join | Include carried multi-day and multileg order state if this profile is ever selected. |
| P2-L308 | L3 process restart | Conditional preflight oracle is not production restart qualification. |
| P2-L309 | LifeNum during bootstrap | Discard partial staging and old active generation atomically. |
| P2-L310 | Resync after invalid state | No stale book or checkpoint advance until bounded recovery succeeds. |
| P2-L311 | Snapshot failure/partial publication | Never publish an incomplete generation. |
| P2-L312 | Catch-up/readiness | Require valid basis, covered sequence, transaction commit, and zero backlog; ONLINE alone is insufficient. |
| P2-L313 | Public identity scope | Freeze identity/reuse rules only with authoritative source evidence; do not invent global uniqueness. |
| P2-L314 | Action/remainder invariants | Preserve unknown action codes; contradictions invalidate the book. |
| P2-L315 | ORDBOOK ClearDeleted | Invalidate ambiguous affected state; never synthesize order cancellation. |
| P2-PF01 | Full-path 100k sustained qualification | Historical raw-only ~99,978 msg/s on Apple M4 Pro is not full-path/Linux evidence. |
| P2-PF02 | Engineering 200k sustained target | Historical raw-only ~199,980 msg/s is not full-path/Linux evidence. |
| P2-PF03 | Engineering 300k burst target | Historical short raw burst is not a full-book burst result. |
| P2-PF04 | Deep-book correctness | No full active-book benchmark/oracle; optional L3 phase. |
| P2-PF05 | Slow optional consumers | Require explicit lost sequence/incomplete state and bounded memory. |
| P2-PF06 | Mandatory backpressure | Mandatory overflow fails health and revokes checkpoint eligibility. |
| P2-PF07 | AGGR/private/publisher coexistence | Target-host combined load, deadlines, CPU/RSS and queue metrics absent. |
| P2-PF08 | Machine-readable benchmark evidence | Historic raw_d0.json is synthetic raw-path evidence; performance pass remains false. |
| P2-API01 | Raw event batch API | Optional ORDLOG API; bounded ownership/overflow/exact fields required. |
| P2-API02 | L3 state/status API | Optional; expose generation/checkpoint/unavailable/stale distinctions. |
| P2-API03 | Generation-scoped view lifetime | Optional; stale views must expire and retained memory be bounded. |
| P2-API04 | Managed batch consumption | Optional; prove bounded batch API without per-record interop overhead. |
| P2-API05 | V1/V2 compatibility | Preserve ABI layouts/semantics; historic ATEST is not current branch certification. |
| P2-EV01 | Certification scenarios/logs | Versioned manifests must map every scenario to immutable exact-source evidence; current live bundle is partial. |
| P2-EV02 | Scheme/commit/config/runtime freeze | Freeze source, binary, library, header, scheme, config, and benchmark host for final candidate; stale manifest cannot be reused. |
| P2-EV03 | Replay recovery oracle | C2 test-only oracle does not replace production snapshot/partial/restart equality. |

The 12 P2-OL rows and 15 P2-L3 rows are all explicitly mapped above and remain DEFERRED_SCOPE for this certificate profile. An independent check confirmed all 96 P2 source IDs are present with
none missing. Open PRs #39, #43, and #44 are tracked as separate source work. Their existence and CI do not change the present scope, acceptance, or external T1 evidence.

### Independent review Cards 01–13 map

| Card | Review topic | Register mapping and disposition |
| --- | --- | --- |
| 01 | Live DTC 507 contract | RO-03/04; producer and offline schema tests pass. Final 507 appeared in bounded prior live sessions; new-candidate live receipt pending. |
| 02 | Definition/source mapping | RO-01/04; bind selected contract, current REFDATA, and definition fields; fresh gate is historical and a sealed candidate receipt remains required. |
| 03 | Metadata revocation | RO-05/06; offline gates/regressions exist. Preserve quiet-source and repeated-AVAILABLE results; current live revoke/reconnect proof pending. |
| 04 | Read-only receipts | RO-07/09; retain zero publisher/reply/order surfaces and hashes; existing receipts cover prior four-listener mode, not the new five-listener live mode. |
| 05 | Integrated live-mode offline tests | E-PD register: full macOS Release and focused Linux tests pass. Full Linux qualification and live DEALS remain open. |
| 06 | Questionnaire draft | Questionnaire coverage below; 77 fields and every printed stream checkbox mapped. Refresh identity and the .07 DEALS impact; 14 user answers remain unresolved. |
| 07 | Next live read-only qualification | RO-01–09 and live gate; two provisional snapshots are not AGGR authority or full-session certification. Market is closed; no new live run was made. |
| 08 | Private views/order-state boundary | Later trading gate: selected product needs private order/position views; not enabled or qualified by this read-only demo. |
| 09 | Serial order demonstration | Later trading gate; historic order evidence is not current candidate proof. No order action is authorized by this register. |
| 10 | AGGR certification campaign | AGGR C/R/S/general tables; preserve the two exact NO event-attempt labels and unresolved authority gate. Full external campaign is not established. |
| 11 | Full ORDLOG | P2-OL01–12; optional/out of scope; C1/C2 work is open, with entitlement and production qualification not established. |
| 12 | Storage/relay follow-on | P2-OL/P2-L3/API and capacity rows; future optional architecture slice, not a blanket prerequisite or a claim of absent code. |
| 13 | Final scope/submission | Verdict and formal submission gate; NOT READY until selected full profile, matrices, questionnaire, current evidence, and external rows are closed and reviewed. |

## Questionnaire coverage and checkbox register

The source questionnaire contains 77 answer fields: 20 in 1a–1t, 4 connection fields plus 33 printed stream checkboxes in 2a, 4 in 2b, 5 in 2c, 3 in 2d, 4 in 3a, and one each in 3b, 3c, 3d, and
4. All groups are retained; 2a.stream.14 and 2a.stream.33 are duplicate printed FORTS_REFDATA_REPL entries for one configured service.

For the 2026-09-28 scope refresh, the status tally is 30 implemented, 30 not in scope, 2 planned, 1 implementation in progress (DEALS), and 14 user-input fields. The register remains
DRAFT_NOT_FOR_SUBMISSION. User-input IDs are 1a, 1b, 1c, 1d, 1e, 1j, 1k, 1l, 1m, 1n, 1p, 1r, 1s, and 1t; resolve privately and do not infer legal identity, user purpose, contacts, consent, or
commercial distribution.

### All 33 printed stream checkboxes

- **2a.stream.01**
  - **Stream:** FORTS_TRADE_REPL
  - **Proposed profile status / impact:** TRADING_CONNECTOR_PROFILE only; own-order/trade reconciliation, not public tape.
- **2a.stream.02**
  - **Stream:** FORTS_COMMON_REPL
  - **Proposed profile status / impact:** Not selected; outside this SPECTRA derivative profile.
- **2a.stream.03**
  - **Stream:** FORTS_VM_REPL
  - **Proposed profile status / impact:** Not selected; outside current declared profile.
- **2a.stream.04**
  - **Stream:** FORTS_ORDLOG_REPL
  - **Proposed profile status / impact:** Planned, optional separate Full ORDLOG qualification. C1/C2 PRs exist; do not mark as current supported certificate scope.
- **2a.stream.05**
  - **Stream:** FORTS_AGGRXX_REPL
  -  **Proposed profile status / impact:** Selected as FORTS_AGGR20_REPL, alias Aggr; four-listener demo base and intended trading profile. Preserve exact bounded event-attempt NO labels;
    authority gate unresolved.
- **2a.stream.06**
  - **Stream:** FORTS_VOLAT_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.07**
  - **Stream:** FORTS_DEALS_REPL
  -  **Proposed profile status / impact:** **Optional and offline-validated; not MOEX-qualified.** Default stays four listeners; --public-deals opts into a fifth. Update
    function/stream/topology/persistence answers; do not describe it as accepted certification functionality.
- **2a.stream.08**
  - **Stream:** FORTS_POS_REPL
  - **Proposed profile status / impact:** TRADING_CONNECTOR_PROFILE only; private position state, not read-only DTC demo.
- **2a.stream.09**
  - **Stream:** FORTS_RISKINFOBLACK_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.10**
  - **Stream:** FORTS_FEE_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.11**
  - **Stream:** FORTS_PART_REPL
  - **Proposed profile status / impact:** TRADING_CONNECTOR_PROFILE only; participant limits.
- **2a.stream.12**
  - **Stream:** FORTS_RISKINFOBACH_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.13**
  - **Stream:** FORTS_FEERATE_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.14**
  - **Stream:** FORTS_REFDATA_REPL, first printed entry
  - **Proposed profile status / impact:** Selected by both profiles; client-scheme alias REFDATA.
- **2a.stream.15**
  - **Stream:** FORTS_INFO_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.16**
  - **Stream:** FORTS_BROKER_FEE_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.17**
  - **Stream:** FORTS_MISCINFO_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.18**
  - **Stream:** FORTS_TNPENALTY_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.19**
  - **Stream:** FORTS_BROKER_FEE_PARAMS_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.20**
  - **Stream:** FORTS_MM_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.21**
  - **Stream:** MOEX_RATES_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.22**
  - **Stream:** FORTS_USERORDERBOOK_REPL
  - **Proposed profile status / impact:** TRADING_CONNECTOR_PROFILE only. OrdBook alias/layout equivalence remains unproven; do not include in read-only demo.
- **2a.stream.23**
  - **Stream:** FORTS_CLR_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.24**
  - **Stream:** FORTS_FORECASTIM_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.25**
  - **Stream:** FORTS_ORDBOOK_REPL
  - **Proposed profile status / impact:** Not selected and distinct from FORTS_USERORDERBOOK_REPL.
- **2a.stream.26**
  - **Stream:** RTS_INDEX_REPL
  - **Proposed profile status / impact:** Not selected.
- **2a.stream.27**
  - **Stream:** ASTS (MCX) stock streams
  - **Proposed profile status / impact:** Not selected; outside declared SPECTRA derivatives scope.
- **2a.stream.28**
  - **Stream:** ASTS (MCX) FX streams
  - **Proposed profile status / impact:** Not selected; outside declared SPECTRA derivatives scope.
- **2a.stream.29**
  - **Stream:** RFS_INFO_REPL
  - **Proposed profile status / impact:** Not selected; no RFS profile.
- **2a.stream.30**
  - **Stream:** RFS_FINESLEVEL_REPL
  - **Proposed profile status / impact:** Not selected; no RFS profile.
- **2a.stream.31**
  - **Stream:** RFS_PENALTY_REPL
  - **Proposed profile status / impact:** Not selected; no RFS profile.
- **2a.stream.32**
  - **Stream:** RFS_USERMARKETDATA_REPL
  - **Proposed profile status / impact:** Not selected; no RFS profile.
- **2a.stream.33**
  - **Stream:** FORTS_REFDATA_REPL, second printed entry
  - **Proposed profile status / impact:** Preserve the duplicate form entry; same REFDATA service as .14, not another listener.

SESSIONSTATE and INSTRUMENTSTATE are not printed checkboxes. List them under the form’s other-streams section. The read-only base profile has AGGR20, REFDATA, SESSIONSTATE, and INSTRUMENTSTATE;
optional DEALS adds only the fifth public read-only listener.

### Questionnaire fields affected by optional DEALS

- **1f, 1g**
  -  **Required update before submission:** State that the full intended Connector SPECTRA futures/trading profile is the target. Kairos is only an external demo UI; read-only is not the
    certificate target. Describe DEALS as opt-in and offline-validated, not MOEX-qualified.
- **1h, 2a.i–2a.iv**
  -  **Required update before submission:** Preserve one CGate connection and one DTC demo-client topology; describe the optional fifth listener, not a fifth connection. Maximum customer/client
    counts remain user input where the register says so.
- **2a.stream.07**
  -  **Required update before submission:** Move stale out-of-scope answer to optional implementation/offline-validated; do not claim MOEX qualification before full acceptance and a five-listener
    profile receipt.
- **2b.II–2b.IV**
  - **Required update before submission:** Add DEALS callback/consumer purpose under the same CGate owner; measure actual conn_process cadence instead of inferring it from sleeps.
- **2c.I.1–2c.I.3**
  -  **Required update before submission:** Add the optional server-scheme FORTS_DEALS_REPL URL, snapshot+online behavior, and in-memory/fresh-snapshot restart semantics. Do not claim durable
    replication continuity.
- **2c.II–2c.III**
  -  **Required update before submission:** State the exact consumed DEALS tables and callback interpretation. The scheme’s presence alone does not prove multileg_deal, heartbeat, or sys_events
    consumption.
- **3a.I and 3a.IV**
  - **Required update before submission:** Include any consumed public DEALS heartbeat and public trade information only if implemented and evidenced.
- **1n**
  - **Required update before submission:** Market-data purpose is USER_INPUT; do not infer redistribution or commercial use from adding the stream.
- **2d and 3b–4**
  -  **Required update before submission:** No command, calendar-spread, mass-operation, client-management, or COD capability follows from public DEALS. Keep existing not-in-scope answers unless
    separately implemented and authorized.

## Later trading qualification gate

This section defines future acceptance only. It is not permission to trade, and no order action was taken here. Read-only DTC/Kairos evidence does not qualify the separately armed TEST command
path.

1.  Freeze and review the exact source, Linux binary, CGate library/header, scheme, config, app_name, account/profile, target instrument, and evidence root. Refresh the 77-field form and all
   matrix mappings.
2.  Require a fresh session/target gate, correct REFDATA and participant identity, zero starting position, known own-order census, current clock evidence within ±1 second, allowed rate, exposure
   and quantity limits, and a documented operator stop path.
3.  For any later specifically authorized ordinary lifecycle, require AddOrder 474 plus both correlated business reply 179 and matching private Working evidence; only then DelOrder 461; then both
   reply 177 and matching private Cancelled evidence with zero active own orders; finish with position reconciliation. Each pair is a conjunction and either channel may arrive first.
4.  Treat system replies 99/100, timeout, disconnect, duplicate, partial response, mismatched identity, or uncertain send as non-success/unknown. No blind Add resend, automatic Cancel/flatten,
   inferred zero position, or checkpoint advance. Preserve the unresolved epoch until explicit reconciliation.
5.  Qualify zero-order restart, then a one-Working-order restart, per-listener loss/reopen, LifeNum/ClearDeleted, publisher loss, and the full required trading day. Never use the paused observer
   as a test harness.
6. Coordinate TCS reload/no-reload, alternate access server, or other exchange-controlled scenarios with MOEX. Keep their status MOEX_COORDINATED until their actual receipts exist.

## Build, test, and evidence commands required after implementation freeze

These are repeatable qualification command templates. The logged macOS Release, focused macOS sanitizer, and focused Linux results above are bounded offline results; preserve first failures and
final results for each later frozen-candidate run.

Release build and full CTest, matching the repository CI shape:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel 2
    ctest --test-dir build --output-on-failure

Focused current-work targets include connector_host_test, plaza2_trade_transport_scenarios_test, connector_host_dtc_server_test, connector_host_dtc507_generated_schema_test when .NET is
installed, plaza2_runtime_probe_test, plaza2_scheme_drift_test, plaza2_runtime_adapter_test, plaza2_aggr20_md_validation_test, plaza2_aggr20_md_runner_test, plaza2_publisher_rate_test,
plaza2_trade_reply_decoding_test, and plaza2_order_lifecycle_scenarios_test.

Linux ASan/UBSan/LSan is required for release readiness; macOS detect_leaks=0 is not Linux LeakSanitizer evidence:

    cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
    cmake --build build-asan --parallel 2
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build-asan --output-on-failure -L "sanitizer|plaza2"

Before calling an evidence package sealed, record exact compiler/OS/architecture and command, test counts, skipped/ignored tests, all exit codes, binary/runtime/scheme/config SHA-256 values, and
hash-verify the entire package. Redact credentials and protected URLs while retaining non-secret identity and provenance.

## Release blockers and next evidence

- **1**
  -  **Required closure:** Finish the public-DEALS change and freeze its exact commit. Preserve four listeners by default; prove the five-listener mode is opt-in and read-only. Run focused tests,
    full Linux Release, and Linux ASan/UBSan/LSan.
- **2**
  -  **Required closure:** Refresh the questionnaire answer register and manifest to the actual source/runtime identity. Keep 14 user-owned answers unresolved until supplied. Update the .07 DEALS
    checkbox and impacted fields listed above.
- **3**
  -  **Required closure:** Seal a four-listener DTC receipt and a separate five-listener DEALS receipt, each with startup/open/ONLINE/commit/recovery state, hashes, zero command handles/posts,
    and loss behavior. Do not combine these with the three-stream target gate.
- **4**
  -  **Required closure:** Preserve current 507, definition and source authority in independent machine-readable client receipts. Capture actual Kairos GUI evidence as demo-only, compare
    Connector and client snapshots, and exercise owned reconnect/invalidation.
- **5**
  -  **Required closure:** Resolve the two bounded NO event attempts, INITIAL_OPEN_SESSION_DATA_READY_AFTER_ONLINE and LISTENER_REOPEN_SESSION_DATA_READY_AFTER_ONLINE, through the proper
    qualification procedure. Do not generalize them to all initial sync/recovery or promote corroborated snapshots.
- **6**
  -  **Required closure:** Demonstrate target-Linux capacity and backpressure for the actual selected profile. Existing macOS raw ORDLOG results do not measure AGGR+DEALS, vendor CGate receive
    cost, or public L3.
- **7**
  -  **Required closure:** Keep Full ORDLOG/L3 optional and deferred unless the user separately expands the product certificate scope. Track PR #39/#43/#44 as open C1/C2 work; require their
    integration, target qualification, current entitlement, and evidence before changing that scope.
- **8**
  -  **Required closure:** Run the separately authorized TEST trading campaign only after product scope, exact candidate, profile, risk limits, session, and explicit action permission are
    settled. Keep TCS/reserve-server tests MOEX_COORDINATED.

Formal submission gate: every applicable matrix and questionnaire row has an explicit evidence level and immutable source identity; no NOT_RUN or provisional row is presented as PASS; every
external row is either actually coordinated or honestly pending; and the final package is reviewed before any submission. This register is not a certificate, MOEX acceptance, deployment approval,
or order authorization.

## Source index

Evidence path aliases: `PD` = /Users/pavel/CSharp/MoexConnector/.codex-tmp/public-deals-review-20260928;
`LIVE` = /Users/pavel/CSharp/MoexConnector/.codex-tmp/card07-live-fix-review-20260928;
`AUDIT` = /Users/pavel/Downloads/MOEX_Connector_Audit 2.

-  Current branch source and review files: cert/AGGR_CERT_MATRIX_9_9.md; cert/PLAZA2_CERT_MATRIX.md; cert/aggr_plaza2_certification_manifest_9_9.json;
  docs/review/moex_cgate_questionnaire_draft_9_9_20260919.md; docs/review/moex_cgate_questionnaire_register_9_9_20260919.json; docs/review/plaza2_certification_baseline_20260907.md;
  docs/review/plaza2_certification_audit_20260907.json; docs/review/plaza2_ordlog_schema_audit_20260907.md.
- Current live review: /Users/pavel/CSharp/MoexConnector/.codex-tmp/card07-live-fix-review-20260928/README.md and its gate/ and connector/ subdirectories.
-  Independent review package: /Users/pavel/Downloads/MOEX_Connector_Audit 2/AUDIT_REPORT.md; START_HERE.md; QUESTIONNAIRE_MAPPING.md; SOURCES.md; luna_tasks/06_QUESTIONNAIRE_DRAFT.md;
  luna_tasks/07_NEXT_LIVE_READONLY.md; luna_tasks/10_AGGR_CERT_CAMPAIGN.md; luna_tasks/01_LIVE_507_CONTRACT.md; luna_tasks/03_METADATA_REVOCATION.md;
  luna_tasks/05_INTEGRATED_LIVE_MODE_OFFLINE.md; luna_tasks/09_SERIAL_ORDER_DEMO.md; luna_tasks/11_FULL_ORDLOG.md; luna_tasks/13_FINAL_SCOPE_AND_SUBMISSION.md.
- Cards 01–13 were mapped individually above; their review-package source files are in `AUDIT/luna_tasks/`. Full ORDLOG and C1/C2 are kept separate from DEALS-07.
- Public DEALS contract: docs/plaza2_public_deals_dtc.md; raw wire/time sources: docs/plaza2/PUBLIC_WIRE_9_9_QUALIFICATION.md;
  docs/plaza2/RAW_ORDLOG_C1.md; docs/plaza2/late_join/SUPPORT_REQUEST_RU.md.
-  Precision and timezone: docs/plaza2/PUBLIC_WIRE_9_9_QUALIFICATION.md; docs/plaza2/RAW_ORDLOG_C1.md; docs/plaza2/late_join/SUPPORT_REQUEST_RU.md. Officially documented moment_ns UTC semantics
  do not settle the raw P2TIME moment timezone.
