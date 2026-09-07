# PLAZA II certification traceability matrix

Audit base: `22a9dfd0947ccde4645696974e1270099491e2c6`, 2026-09-07. **Not certification-ready.** Phase A only.

Authority: [MOEX certification procedure](https://www.moex.com/files/4qg0gqtzcxkep68687ah1bwq5e), general sections 2–3 and Appendix 1 Plaza II (printed pages 5–7), retrieved this
audit. `C1–C8`, `R1–R8`, `S1–S7` below map to its connection, replication and sending subsections; `G` refers to general requirements. `PLAN` denotes the attached plan's additional
acceptance criteria, not a separate claimed MOEX quotation. R8 is split into its two controls. No claim that the 2021 public procedure replaces the current MOEX inquiry form.

Each row is independently applicable. **REQUIRED** means software capability/evidence; **MOEX-COORDINATED** means an externally scheduled exercise, not a waiver. Status **PASS** is
restricted to the row's stated scope; **FAIL** is a code-confirmed requirement contradiction; **BLOCKED** means missing implementation or protocol authority; **NOT RUN** means
qualification missing. Historical T1 receipts do not certify current HEAD. No full-order-log or L3 row is N/A.

Evidence map (paths relative to repository root):

<table>
<tr>
<th>Key</th>
<th>Code / tests / artifact</th>
<th>Scope</th>
</tr>
<tr>
<td>RT</td>
<td><code>protocols/plaza2_cgate/src/plaza2_runtime.cpp</code>; <code>tests/plaza2_cgate/plaza2_runtime_adapter_test.cpp</code>, <code>plaza2_scheme_drift_test.cpp</code>,
<code>plaza2_runtime_probe_test.cpp</code></td>
<td>Runtime wrappers, wire callbacks, scheme checks</td>
</tr>
<tr>
<td>LIVE</td>
<td><code>protocols/plaza2_cgate/src/plaza2_live_session_runner.cpp</code>; <code>tests/plaza2_cgate/plaza2_live_session_runner_test.cpp</code></td>
<td>Existing private live orchestration</td>
</tr>
<tr>
<td>PRIVATE</td>
<td><code>protocols/plaza2_cgate/src/plaza2_private_state_bridge.cpp</code>, <code>plaza2_private_state.cpp</code>;
<code>tests/plaza2_cgate/plaza2_private_state_{projection,visibility,invalidation,provenance}_test.cpp</code></td>
<td>Private state only</td>
</tr>
<tr>
<td>AGGR</td>
<td><code>protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp</code>; <code>tests/plaza2_cgate/plaza2_aggr20_md_{projection,runner,validation}_test.cpp</code></td>
<td>Existing independent AGGR20</td>
</tr>
<tr>
<td>TRADE</td>
<td><code>connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp</code>; <code>tests/plaza2_trade/plaza2_trade_transport_scenarios_test.cpp</code></td>
<td>Guarded transport, replies, certainty, initial listener retry</td>
</tr>
<tr>
<td>CODEC</td>
<td><code>connectors/plaza2_trade/src/plaza2_trade_codec.cpp</code>; <code>tests/plaza2_trade/plaza2_trade_reply_decoding_test.cpp</code>, <code>plaza2_trade_command_encoding_test.cpp</code></td>
<td>Offline codec, not live reply acceptance</td>
</tr>
<tr>
<td>HOST</td>
<td><code>connectors/connector_host/</code>, <code>connectors/plaza2_trade/src/plaza2_order_lifecycle.cpp</code>; <code>tests/connector_host_test.cpp</code>,
<code>tests/plaza2_trade/plaza2_order_lifecycle_scenarios_test.cpp</code></td>
<td>Persistent order epochs and restart reconciliation</td>
</tr>
<tr>
<td>META</td>
<td><code>protocols/plaza2_cgate/schema/plaza2_forts_reviewed.ini</code>, <code>generated/plaza2_generated_metadata.*</code>; <code>tests/assert_plaza2_metadata_invariants.py</code></td>
<td>Reviewed logical fields, not frozen wire structs</td>
</tr>
<tr>
<td>T99</td>
<td><code>docs/evidence/plaza2_cgate99_t1_runtime_qualification_20260901/cgate99_connector_parity_receipt.json</code></td>
<td>Historical source e02694f; 8 streams + publisher bring-up</td>
</tr>
<tr>
<td>TORDER</td>
<td><code>docs/evidence/plaza2_test_order_success_20260906/{README.md,journal.json,execution_safety_v4.json,observation.log}</code></td>
<td>Historical source ffc5df7; one Add/Cancel, TRADE terminal; no final independent UOB census</td>
</tr>
<tr>
<td>ATEST</td>
<td><code>docs/review/plaza2_certification_audit_20260907.json</code></td>
<td>Current 163/163 offline Release run, 54 PLAZA-labelled tests; subsequent preflight recorded separately</td>
</tr>
<tr>
<td>SCHEMA</td>
<td><code>docs/review/plaza2_ordlog_schema_audit_20260907.md</code></td>
<td>10/10 local 9.9 manual field match; current official binary verification BLOCKED</td>
</tr>
</table>

Details, sources, proposed types, recovery protocol gates and performance measurement are in the [baseline](../docs/review/plaza2_certification_baseline_20260907.md). Test names
below denote existing coverage when stated; “add” means a future test, never a passing test. T1 “none” means no sufficient artifact found in audited committed bundles. New T1
scenarios and benchmark evidence paths are prospective, not executed artifacts.

## Official requirements

<table>
<tr>
<th>ID / authority</th>
<th>Requirement / declared capability</th>
<th>Applicability</th>
<th>Implementation / code</th>
<th>Offline test / evidence</th>
<th>T1 evidence / required test</th>
<th>Status</th>
<th>Exact blocker / next work</th>
</tr>
<tr>
<td>P2-C01 / C1</td>
<td>Connection URLs</td>
<td>REQUIRED</td>
<td>RT/LIVE implemented</td>
<td>RT + ATEST</td>
<td>T99 partial; bind inquiry URLs</td>
<td>NOT RUN</td>
<td>Current inquiry form and exact certification URL log</td>
</tr>
<tr>
<td>P2-C02 / C2</td>
<td>Connection thread ownership</td>
<td>REQUIRED</td>
<td>Single pump in LIVE/TRADE</td>
<td>Code reviewed; add ownership assertion</td>
<td>None; record owner/thread mapping</td>
<td>NOT RUN</td>
<td>Thread/connection declaration and runtime evidence</td>
</tr>
<tr>
<td>P2-C03 / C3</td>
<td>Idle polling for at least five minutes</td>
<td>REQUIRED</td>
<td>RT process handles timeout; loops present</td>
<td>runtime_adapter timeout test; no 300 s idle test</td>
<td>None; 300 s idle streamless run</td>
<td>NOT RUN</td>
<td>Dedicated idle-only evidence, not active multistream duration</td>
</tr>
<tr>
<td>P2-C04 / C4</td>
<td>Connect with authenticated router</td>
<td>REQUIRED</td>
<td>RT/LIVE implemented</td>
<td>RT/LIVE + ATEST</td>
<td>T99 archived bring-up</td>
<td>PASS</td>
<td>PASS for historical bring-up only; repeat at freeze</td>
</tr>
<tr>
<td>P2-C05 / C5</td>
<td>Router up but exchange disconnected</td>
<td>REQUIRED</td>
<td>State wrappers, partial orchestration</td>
<td>Add precise disconnected-router fixture</td>
<td>None; router without exchange session</td>
<td>NOT RUN</td>
<td>Demonstrate wait/no-send state and recovery</td>
</tr>
<tr>
<td>P2-C06 / C6</td>
<td>Detect exchange connection becoming available</td>
<td>REQUIRED</td>
<td>LIVE/TRADE start logic</td>
<td>Initial asynchronous open tests in TRADE</td>
<td>T99 initial transitions only</td>
<td>NOT RUN</td>
<td>Prove disconnected-to-connected transition while running</td>
</tr>
<tr>
<td>P2-C07 / C7</td>
<td>Exchange-network loss suspends activity</td>
<td>REQUIRED</td>
<td>Fail-closed checks, incomplete supervision</td>
<td>TRADE close/uncertainty tests are partial</td>
<td>None; controlled upstream interruption</td>
<td>BLOCKED</td>
<td>Full mid-run detection/recovery evidence</td>
</tr>
<tr>
<td>P2-C08 / C8</td>
<td>Router connection loss suspends activity</td>
<td>REQUIRED</td>
<td>RT errors propagate</td>
<td>Add router loss/recovery integration</td>
<td>None; controlled router interruption</td>
<td>NOT RUN</td>
<td>Exact outage and resumed-state logs</td>
</tr>
<tr>
<td>P2-R01 / R1</td>
<td>Subscription URLs/opening</td>
<td>REQUIRED</td>
<td>RT/LIVE existing; public path absent</td>
<td>Existing listener tests + ATEST</td>
<td>T99 private/status/AGGR only</td>
<td>BLOCKED</td>
<td>ORDLOG/ORDBOOK URLs + declared stream list</td>
</tr>
<tr>
<td>P2-R02 / R2</td>
<td>Subscription thread rules</td>
<td>REQUIRED</td>
<td>Single-owner callbacks; public design pending</td>
<td>Add same-owner/coexistence fixture</td>
<td>None; include subscriptions per connection</td>
<td>NOT RUN</td>
<td>Freeze public subscription topology</td>
</tr>
<tr>
<td>P2-R03 / R3</td>
<td>Valid receive schemes</td>
<td>REQUIRED</td>
<td>META + RT; consumed profile only</td>
<td>Scheme drift/metadata tests + SCHEMA</td>
<td>T99 existing consumed subset</td>
<td>BLOCKED</td>
<td>Freeze all new 9.9 public byte layouts/nulls/indices</td>
</tr>
<tr>
<td>P2-R04 / R4</td>
<td>Compatible scheme evolution</td>
<td>REQUIRED</td>
<td>RT compatibility checks</td>
<td>plaza2_scheme_drift_test + ATEST</td>
<td>None; coordinated additive field/table change</td>
<td>NOT RUN</td>
<td>Public required profile and negotiated unknown-field preservation</td>
</tr>
<tr>
<td>P2-R05 / R5</td>
<td>Incompatible scheme diagnostics</td>
<td>REQUIRED</td>
<td>RT fail-closed drift checks</td>
<td>plaza2_scheme_drift_test + ATEST</td>
<td>None; coordinated incompatible scheme exercise</td>
<td>NOT RUN</td>
<td>Extend required public fields; retain exact mismatch diagnostics</td>
</tr>
<tr>
<td>P2-R06 / R6</td>
<td>Every listener reopens correctly</td>
<td>REQUIRED</td>
<td>TRADE initial-open retry only</td>
<td>TRADE initial ERROR/reopen tests</td>
<td>None; per-stream mid-run interruption</td>
<td>BLOCKED</td>
<td>Post-snapshot ERROR explicitly fails; implement recovery</td>
</tr>
<tr>
<td>P2-R07 / R7</td>
<td>Full ORDLOG &gt;=100,000 messages/s</td>
<td>REQUIRED</td>
<td>No live full path or benchmark</td>
<td>Add Phase D paced full-path benchmark</td>
<td>None; certify on declared machine</td>
<td>BLOCKED</td>
<td>100k without growing delay/loss; 200k is internal only</td>
</tr>
<tr>
<td>P2-R08A / R8a</td>
<td>ClearDeleted table/range semantics</td>
<td>REQUIRED</td>
<td>RT payload + PRIVATE scope; AGGR reset</td>
<td>Private invalidation/provenance tests partial</td>
<td>None; deletion ranges per declared stream</td>
<td>FAIL</td>
<td>AGGR blanket reset; ORDLOG/ORDBOOK absent; verify range predicate</td>
</tr>
<tr>
<td>P2-R08B / R8b</td>
<td>LifeNum generation semantics</td>
<td>REQUIRED</td>
<td>PRIVATE scoped invalidation; AGGR differs</td>
<td>Private and TRADE LifeNum tests partial</td>
<td>T99 observed life values, not life change exercise</td>
<td>FAIL</td>
<td>Standalone AGGR ignores LifeNum; public generation absent</td>
</tr>
<tr>
<td>P2-S01 / S1</td>
<td>Publisher URLs/opening</td>
<td>REQUIRED</td>
<td>RT/TRADE implemented</td>
<td>runtime_adapter + transport tests</td>
<td>T99 and TORDER archived</td>
<td>PASS</td>
<td>Historical bounded open proof; freeze inquiry URLs later</td>
</tr>
<tr>
<td>P2-S02 / S2</td>
<td>Publisher thread rules</td>
<td>REQUIRED</td>
<td>Single host owner in TRADE</td>
<td>Code review; add ownership fixture</td>
<td>None; declaration/thread log</td>
<td>NOT RUN</td>
<td>Owner/thread enforcement under combined load</td>
</tr>
<tr>
<td>P2-S03 / S3</td>
<td>Configurable command rate gate</td>
<td>REQUIRED</td>
<td>Absent in PLAZA</td>
<td>Add boundary/reopen/timeout accounting tests</td>
<td>None; exercise provisioned rate</td>
<td>FAIL</td>
<td>A low send count is not a configurable gate</td>
</tr>
<tr>
<td>P2-S04 / S4</td>
<td>Valid send scheme</td>
<td>REQUIRED</td>
<td>Official 9.9 CODEC fixtures</td>
<td>CODEC command validation/encoding + ATEST</td>
<td>TORDER Add/Del only</td>
<td>PASS</td>
<td>Scope Add/Del only; qualify every additional declared command</td>
</tr>
<tr>
<td>P2-S05 / S5</td>
<td>Replies and timeouts terminate safely</td>
<td>REQUIRED</td>
<td>TRADE operation correlation/uncertainty</td>
<td>TRADE timeout and lifecycle tests + ATEST</td>
<td>TORDER ordinary reply only; timeout exercise missing</td>
<td>NOT RUN</td>
<td>Bounded T1 timeout/ambiguity evidence</td>
</tr>
<tr>
<td>P2-S06 / S6</td>
<td>Replies 99 and 100</td>
<td>REQUIRED</td>
<td>CODEC decodes; TRADE rejects before codec</td>
<td>CODEC decode tests PASS; live classification FAIL</td>
<td>None; end-to-end system/flood scenarios</td>
<td>FAIL</td>
<td>Fix ReplyBridge and second whitelist, preserve certainty/no resend</td>
</tr>
<tr>
<td>P2-S07 / S7</td>
<td>Publisher error detection/reopen</td>
<td>REQUIRED</td>
<td>Open/close wrappers; no supervisor located</td>
<td>Add loss/reopen with outstanding operation</td>
<td>None; controlled publisher loss</td>
<td>BLOCKED</td>
<td>Track live state and bounded reopen without retransmission</td>
</tr>
<tr>
<td>P2-G01 / G2</td>
<td>Operation/result audit logs</td>
<td>REQUIRED</td>
<td>Receipts/journals/runners present</td>
<td>HOST/TRADE evidence tests + ATEST</td>
<td>TORDER bounded run</td>
<td>NOT RUN</td>
<td>Full stream/control audit and complete declared command coverage</td>
</tr>
<tr>
<td>P2-G02 / G2</td>
<td>Network interruption recovery</td>
<td>REQUIRED</td>
<td>Partial fail-closed behavior</td>
<td>Add common full recovery sequence</td>
<td>None; outage exercise</td>
<td>NOT RUN</td>
<td>State/checkpoint equivalence after real outage</td>
</tr>
<tr>
<td>P2-G03 / G2</td>
<td>Application restart during session</td>
<td>REQUIRED</td>
<td>HOST orders implemented; public L3 absent</td>
<td>HOST restart tests; add L3 equality</td>
<td>None; current mid-session restart bundle</td>
<td>BLOCKED</td>
<td>Fresh public snapshot or atomic state/checkpoint resume</td>
</tr>
<tr>
<td>P2-G04 / G2</td>
<td>TCS restart with data reload</td>
<td>MOEX-COORDINATED</td>
<td>Public generation recovery absent</td>
<td>Add reload/LifeNum replay</td>
<td>None; MOEX scheduled reload</td>
<td>BLOCKED</td>
<td>Software recovery then exchange-coordinated evidence</td>
</tr>
<tr>
<td>P2-G05 / G2</td>
<td>TCS restart without data reload</td>
<td>MOEX-COORDINATED</td>
<td>Public checkpoint recovery absent</td>
<td>Add no-reload replay</td>
<td>None; MOEX scheduled restart</td>
<td>BLOCKED</td>
<td>Safe retained-history replay and coordinated evidence</td>
</tr>
<tr>
<td>P2-G06 / G2</td>
<td>Reserve/backup access-server switch</td>
<td>MOEX-COORDINATED</td>
<td>No qualified alternate path found</td>
<td>Add configuration validation, no secret export</td>
<td>None; provision/coordinate alternate access</td>
<td>NOT RUN</td>
<td>Exact endpoints/configuration and failover evidence</td>
</tr>
<tr>
<td>P2-G07 / G2</td>
<td>Administration/monitoring if broker use</td>
<td>REQUIRED</td>
<td>Operator CLI/receipts exist</td>
<td>HOST/operator tests partial</td>
<td>None for broker administration scope</td>
<td>NOT RUN</td>
<td>Resolve actual broker-use declaration; no inferred N/A</td>
</tr>
<tr>
<td>P2-G08 / G2</td>
<td>Exchange terminology mapping</td>
<td>REQUIRED</td>
<td>META/CODEC names preserved</td>
<td>Metadata invariants + ATEST</td>
<td>Inquiry mapping not archived</td>
<td>NOT RUN</td>
<td>Freeze declared commands/tables with one-to-one names</td>
</tr>
<tr>
<td>P2-G09 / G2</td>
<td>Selected subsystem routing</td>
<td>REQUIRED</td>
<td>Guarded target/account/identity checks</td>
<td>TRADE mismatch/no-send tests</td>
<td>TORDER single SPECTRA TEST target</td>
<td>NOT RUN</td>
<td>Scope declaration and complete routing evidence</td>
</tr>
<tr>
<td>P2-G10 / G3</td>
<td>Router logging configuration</td>
<td>REQUIRED</td>
<td>Historical receipts, config not committed</td>
<td>Add redacted config fingerprint verification</td>
<td>None for final default logging configuration</td>
<td>NOT RUN</td>
<td>Default router log level plus complete certification logs</td>
</tr>
<tr>
<td>P2-G11 / G3</td>
<td>Full trading day including clearing/evening</td>
<td>MOEX-COORDINATED</td>
<td>Not qualified</td>
<td>Add long-run tool/bounds first</td>
<td>None; scheduled full-day run</td>
<td>NOT RUN</td>
<td>All declared commands, full day and clearing evidence</td>
</tr>
<tr>
<td>P2-G12 / G3</td>
<td>Application/inquiry form and test agreement</td>
<td>MOEX-COORDINATED</td>
<td>Not archived for new full-log scope</td>
<td>Document audit only</td>
<td>MOEX inquiry and agreed testing schedule missing</td>
<td>NOT RUN</td>
<td>Submit separately when authorized; no outreach in Phase A</td>
</tr>
</table>

## Plan refinements and implementation gates

<table>
<tr>
<th>ID / authority</th>
<th>Requirement / declared capability</th>
<th>Applicability</th>
<th>Implementation / code</th>
<th>Offline test / evidence</th>
<th>T1 evidence / required test</th>
<th>Status</th>
<th>Exact blocker / next work</th>
</tr>
<tr>
<td>P2-RT01 / PLAN</td>
<td>CGate environment initialization</td>
<td>REQUIRED</td>
<td>RT implemented</td>
<td>runtime_probe/runtime_adapter + ATEST</td>
<td>T99 archived</td>
<td>PASS</td>
<td>Only existing bring-up scope</td>
</tr>
<tr>
<td>P2-RT02 / PLAN</td>
<td>Router process restart</td>
<td>REQUIRED</td>
<td>Partial wrappers only</td>
<td>Add close/restart/reopen fixture</td>
<td>None; controlled router restart</td>
<td>NOT RUN</td>
<td>Bounded full recovery, fresh state</td>
</tr>
<tr>
<td>P2-RT03 / PLAN</td>
<td>Access-server interruption</td>
<td>MOEX-COORDINATED</td>
<td>No qualified sequence</td>
<td>Add error/fallback fixture</td>
<td>None; coordinate server interruption</td>
<td>NOT RUN</td>
<td>Failure detection and recovery logs</td>
</tr>
<tr>
<td>P2-RP01 / PLAN</td>
<td>Listener error and close status</td>
<td>REQUIRED</td>
<td>RT/LIVE implemented</td>
<td>RT and TRADE close tests + ATEST</td>
<td>T99 initial transitions only</td>
<td>NOT RUN</td>
<td>All public listeners and failure-reason evidence</td>
</tr>
<tr>
<td>P2-RP02 / PLAN</td>
<td>Online transition</td>
<td>REQUIRED</td>
<td>Existing RT/LIVE implemented</td>
<td>LIVE/PRIVATE/AGGR tests + ATEST</td>
<td>T99 archived</td>
<td>PASS</td>
<td>Only existing streams; L3 Ready is separate</td>
</tr>
<tr>
<td>P2-RP03 / PLAN</td>
<td>Replstate recovery</td>
<td>REQUIRED</td>
<td>In-memory markers/fake replay only</td>
<td>resume_from_replstate.yaml</td>
<td>No durable public restart evidence</td>
<td>BLOCKED</td>
<td>Per-stream durable applied boundaries; no marker-only empty-book resume</td>
</tr>
<tr>
<td>P2-RP04 / PLAN</td>
<td>Durable checkpoint after mandatory apply</td>
<td>REQUIRED</td>
<td>No public durable state store</td>
<td>Add fault at decode/apply/commit/publish</td>
<td>None</td>
<td>BLOCKED</td>
<td>Atomic state/checkpoint pair or explicitly fresh bootstrap</td>
</tr>
<tr>
<td>P2-TX01 / PLAN</td>
<td>No blind retransmission after ambiguous send</td>
<td>REQUIRED</td>
<td>TRADE + HOST implemented</td>
<td>PossiblySent, timeout, repeated-send guard tests</td>
<td>TORDER one Add/one Del, no recovery post</td>
<td>PASS</td>
<td>Offline safety + bounded historical run, no general timeout T1 claim</td>
</tr>
<tr>
<td>P2-TX02 / PLAN</td>
<td>Command-specific reply correlation</td>
<td>REQUIRED</td>
<td>TRADE 179/177/186 implemented</td>
<td>TRADE wrong-user/family/duplicate tests</td>
<td>TORDER Add/Del accepted</td>
<td>PASS</td>
<td>Additional declared command replies still require qualification</td>
</tr>
<tr>
<td>P2-TX03 / PLAN</td>
<td>Add -&gt; Working -&gt; Cancel -&gt; Cancelled</td>
<td>REQUIRED</td>
<td>TRADE private lifecycle implemented</td>
<td>Lifecycle/transport + ATEST</td>
<td>TORDER archived successful bounded run</td>
<td>PASS</td>
<td>Preserve exact path; new freeze rerun later</td>
</tr>
<tr>
<td>P2-TX04 / PLAN</td>
<td>Persistent serial/epoch and restart reconciliation</td>
<td>REQUIRED</td>
<td>HOST + order lifecycle</td>
<td>HOST restart/epoch and C ABI V3 + ATEST</td>
<td>No multi-epoch T1 qualification</td>
<td>NOT RUN</td>
<td>Run guarded persistent restart; do not equate order journal to L3 checkpoint</td>
</tr>
<tr>
<td>P2-TX05 / PLAN</td>
<td>Rate boundary/reconnect/timeout handling</td>
<td>REQUIRED</td>
<td>Rate gate absent</td>
<td>Add deterministic monotonic-clock tests</td>
<td>None</td>
<td>BLOCKED</td>
<td>Shared budget, post accounting, limits; dynamic updates unsupported</td>
</tr>
<tr>
<td>P2-MD01 / PLAN</td>
<td>AGGR20 initial synchronization</td>
<td>REQUIRED</td>
<td>AGGR + TRADE implemented</td>
<td>AGGR projection/runner + ATEST</td>
<td>T99, TORDER BBO</td>
<td>PASS</td>
<td>Bounded existing product only</td>
</tr>
<tr>
<td>P2-MD02 / PLAN</td>
<td>AGGR20 online operation</td>
<td>REQUIRED</td>
<td>AGGR exact levels/freshness</td>
<td>AGGR + TRADE multi-instrument tests</td>
<td>T99/TORDER</td>
<td>PASS</td>
<td>Historical bounded online proof</td>
</tr>
<tr>
<td>P2-MD03 / PLAN</td>
<td>AGGR20 recovery</td>
<td>REQUIRED</td>
<td>Reset/restart partial</td>
<td>TRADE closure/restart tests partial</td>
<td>None for network/TCS recovery</td>
<td>NOT RUN</td>
<td>Prove fresh synchronization and no retained stale levels</td>
</tr>
<tr>
<td>P2-MD04 / PLAN</td>
<td>AGGR20 LifeNum</td>
<td>REQUIRED</td>
<td>Standalone ignored; transport resets</td>
<td>Add standalone generation-reset test</td>
<td>No life-change exercise</td>
<td>FAIL</td>
<td>Standalone bridge must invalidate book/readiness</td>
</tr>
<tr>
<td>P2-MD05 / PLAN</td>
<td>AGGR20 ClearDeleted</td>
<td>REQUIRED</td>
<td>Both bridges clear entire book</td>
<td>Add table/revision survival fixture</td>
<td>None</td>
<td>FAIL</td>
<td>Verify/apply actual range semantics, not blanket clear</td>
</tr>
<tr>
<td>P2-OL01 / PLAN</td>
<td>FORTS_ORDLOG_REPL live listener</td>
<td>REQUIRED</td>
<td>META only; pipeline absent</td>
<td>SCHEMA partial; add C1/C2 deterministic fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Add dedicated public listener; preserve AGGR/private</td>
</tr>
<tr>
<td>P2-OL02 / PLAN</td>
<td>Exact ORDLOG scheme</td>
<td>REQUIRED</td>
<td>META only; pipeline absent</td>
<td>SCHEMA partial; add C1/C2 deterministic fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Current official 9.9 sizes/offsets/index/nullability unavailable</td>
</tr>
<tr>
<td>P2-OL03 / PLAN</td>
<td>Lossless ORDLOG decoding</td>
<td>REQUIRED</td>
<td>META only; pipeline absent</td>
<td>SCHEMA partial; add C1/C2 deterministic fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Callback strips raw payload/null bitmap, textifies prices, truncates time</td>
</tr>
<tr>
<td>P2-OL04 / PLAN</td>
<td>Revision semantics and applied checkpoints</td>
<td>REQUIRED</td>
<td>META only; pipeline absent</td>
<td>SCHEMA partial; add C1/C2 deterministic fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Verify per-table revision rules; no consecutive integer assumption</td>
</tr>
<tr>
<td>P2-OL05 / PLAN</td>
<td>Raw replay and full event output</td>
<td>REQUIRED</td>
<td>META only; pipeline absent</td>
<td>SCHEMA partial; add C1/C2 deterministic fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Own bounded wire batches; preserve every table/control event</td>
</tr>
<tr>
<td>P2-OL06 / PLAN</td>
<td>Duplicate handling</td>
<td>REQUIRED</td>
<td>META only; pipeline absent</td>
<td>SCHEMA partial; add C1/C2 deterministic fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Equal revision+bytes idempotent; contradictions resync</td>
</tr>
<tr>
<td>P2-OL07 / PLAN</td>
<td>Gap handling</td>
<td>REQUIRED</td>
<td>META only; pipeline absent</td>
<td>SCHEMA partial; add C1/C2 deterministic fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Verified retention/coverage detection; fail closed</td>
</tr>
<tr>
<td>P2-OL08 / PLAN</td>
<td>ORDLOG LifeNum</td>
<td>REQUIRED</td>
<td>META only; pipeline absent</td>
<td>SCHEMA partial; add C1/C2 deterministic fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Invalidate generation, unsafe views and markers</td>
</tr>
<tr>
<td>P2-OL09 / PLAN</td>
<td>ORDLOG ClearDeleted</td>
<td>REQUIRED</td>
<td>META only; pipeline absent</td>
<td>SCHEMA partial; add C1/C2 deterministic fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Expire replicated history independently of active L3 orders</td>
</tr>
<tr>
<td>P2-OL10 / PLAN</td>
<td>ORDLOG restart recovery</td>
<td>REQUIRED</td>
<td>META only; pipeline absent</td>
<td>SCHEMA partial; add C1/C2 deterministic fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Fresh bootstrap or verified durable state+marker</td>
</tr>
<tr>
<td>P2-OL11 / PLAN</td>
<td>ORDLOG TCS no-reload recovery</td>
<td>REQUIRED</td>
<td>META only; pipeline absent</td>
<td>SCHEMA partial; add C1/C2 deterministic fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Retained-history sequence and hash equality</td>
</tr>
<tr>
<td>P2-OL12 / PLAN</td>
<td>ORDLOG TCS reload recovery</td>
<td>REQUIRED</td>
<td>META only; pipeline absent</td>
<td>SCHEMA partial; add C1/C2 deterministic fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>New generation and full rebootstrap</td>
</tr>
<tr>
<td>P2-L301 / PLAN</td>
<td>FORTS_ORDBOOK_REPL snapshot</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Six tables exist only as metadata; stage complete publication</td>
</tr>
<tr>
<td>P2-L302 / PLAN</td>
<td>Bootstrap</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Seed active remainder; do not replay snapshot as order execution</td>
</tr>
<tr>
<td>P2-L303 / PLAN</td>
<td>Snapshot revision and LifeNum</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Bind trades_rev/trades_lifenum to matching log and publication</td>
</tr>
<tr>
<td>P2-L304 / PLAN</td>
<td>ORDLOG handoff</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Resolve stale logRev example and verify actual 9.9 open settings</td>
</tr>
<tr>
<td>P2-L305 / PLAN</td>
<td>No snapshot/log race gap</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Proof of retained incremental coverage after boundary</td>
</tr>
<tr>
<td>P2-L306 / PLAN</td>
<td>Deterministic final book/hash</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Canonical complete active state plus checkpoint; not BBO only</td>
</tr>
<tr>
<td>P2-L307 / PLAN</td>
<td>Late join</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Include carried multi-day orders and both multileg tables</td>
</tr>
<tr>
<td>P2-L308 / PLAN</td>
<td>L3 process restart</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Hash equality with uninterrupted path</td>
</tr>
<tr>
<td>P2-L309 / PLAN</td>
<td>LifeNum during bootstrap</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Discard staging and old active generation</td>
</tr>
<tr>
<td>P2-L310 / PLAN</td>
<td>Resync after invalid state</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Bounded retries, unavailable stale book, no checkpoint advance</td>
</tr>
<tr>
<td>P2-L311 / PLAN</td>
<td>Snapshot failure/partial publication</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Never publish incomplete generation</td>
</tr>
<tr>
<td>P2-L312 / PLAN</td>
<td>Catch-up completion/readiness</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Valid basis, coverage, commit and zero backlog; ONLINE alone insufficient</td>
</tr>
<tr>
<td>P2-L313 / PLAN</td>
<td>Public identity scope</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Verify uniqueness and rollover continuity before freezing key</td>
</tr>
<tr>
<td>P2-L314 / PLAN</td>
<td>Action/remainder invariants</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Unknown actions preserved raw; contradictions invalidate L3</td>
</tr>
<tr>
<td>P2-L315 / PLAN</td>
<td>ORDBOOK ClearDeleted</td>
<td>REQUIRED</td>
<td>Absent; proposed native C2</td>
<td>Add C2 replay/fault fixtures</td>
<td>None</td>
<td>BLOCKED</td>
<td>Publication-aware table/range semantics; preserve valid active state</td>
</tr>
<tr>
<td>P2-PF01 / PLAN</td>
<td>Full-path 100k sustained qualification</td>
<td>REQUIRED</td>
<td>Benchmark absent</td>
<td>Add D benchmark modes; no parser-only claims</td>
<td>None</td>
<td>BLOCKED</td>
<td>Paced wire input through decode/raw/L3/commit; no lag growth</td>
</tr>
<tr>
<td>P2-PF02 / PLAN</td>
<td>Engineering 200k sustained target</td>
<td>REQUIRED</td>
<td>Benchmark absent</td>
<td>Add D benchmark modes; no parser-only claims</td>
<td>None</td>
<td>BLOCKED</td>
<td>Internal target, not official requirement</td>
</tr>
<tr>
<td>P2-PF03 / PLAN</td>
<td>Engineering 300k burst target</td>
<td>REQUIRED</td>
<td>Benchmark absent</td>
<td>Add D benchmark modes; no parser-only claims</td>
<td>None</td>
<td>BLOCKED</td>
<td>Bounded queue and measured recovery time</td>
</tr>
<tr>
<td>P2-PF04 / PLAN</td>
<td>Deep-book correctness</td>
<td>REQUIRED</td>
<td>Benchmark absent</td>
<td>Add D benchmark modes; no parser-only claims</td>
<td>None</td>
<td>BLOCKED</td>
<td>Large active populations; deterministic full-state oracle</td>
</tr>
<tr>
<td>P2-PF05 / PLAN</td>
<td>Slow optional consumers</td>
<td>REQUIRED</td>
<td>Benchmark absent</td>
<td>Add D benchmark modes; no parser-only claims</td>
<td>None</td>
<td>BLOCKED</td>
<td>Bounded batches; explicit overflow/loss boundary and invalidation</td>
</tr>
<tr>
<td>P2-PF06 / PLAN</td>
<td>Mandatory backpressure</td>
<td>REQUIRED</td>
<td>Benchmark absent</td>
<td>Add D benchmark modes; no parser-only claims</td>
<td>None</td>
<td>BLOCKED</td>
<td>Fail unhealthy and stop checkpoint; never silently drop</td>
</tr>
<tr>
<td>P2-PF07 / PLAN</td>
<td>AGGR/private/publisher coexistence</td>
<td>REQUIRED</td>
<td>Benchmark absent</td>
<td>Add D benchmark modes; no parser-only claims</td>
<td>None</td>
<td>BLOCKED</td>
<td>Combined pump load and transaction reply deadlines</td>
</tr>
<tr>
<td>P2-PF08 / PLAN</td>
<td>Machine-readable benchmark evidence</td>
<td>REQUIRED</td>
<td>Benchmark absent</td>
<td>Add D benchmark modes; no parser-only claims</td>
<td>None</td>
<td>BLOCKED</td>
<td>Build/machine/corpus/rate/latency/backlog/RSS/hash/drop counters</td>
</tr>
<tr>
<td>P2-API01 / PLAN</td>
<td>Raw event batch API</td>
<td>REQUIRED</td>
<td>V1/V2/V3 exist; public API absent</td>
<td>Add E ownership/overflow/ABI tests</td>
<td>None</td>
<td>BLOCKED</td>
<td>Owned or leased native batches; all exact fields and envelope</td>
</tr>
<tr>
<td>P2-API02 / PLAN</td>
<td>L3 state/status API</td>
<td>REQUIRED</td>
<td>V1/V2/V3 exist; public API absent</td>
<td>Add E ownership/overflow/ABI tests</td>
<td>None</td>
<td>BLOCKED</td>
<td>Generation, life, checkpoint and unavailable/stale semantics</td>
</tr>
<tr>
<td>P2-API03 / PLAN</td>
<td>Generation-scoped view lifetime</td>
<td>REQUIRED</td>
<td>V1/V2/V3 exist; public API absent</td>
<td>Add E ownership/overflow/ABI tests</td>
<td>None</td>
<td>BLOCKED</td>
<td>Expire stale handles; explicit release; bounded retention</td>
</tr>
<tr>
<td>P2-API04 / PLAN</td>
<td>Managed batch consumption</td>
<td>REQUIRED</td>
<td>V1/V2/V3 exist; public API absent</td>
<td>Add E ownership/overflow/ABI tests</td>
<td>None</td>
<td>BLOCKED</td>
<td>Additive API after native proof; no per-record P/Invoke</td>
</tr>
<tr>
<td>P2-API05 / PLAN</td>
<td>Existing V1/V2 compatibility</td>
<td>REQUIRED</td>
<td>Existing ABI unchanged</td>
<td>ATEST</td>
<td>None</td>
<td>PASS</td>
<td>Preserve layouts/semantics; current ABI regression passes</td>
</tr>
<tr>
<td>P2-EV01 / PLAN</td>
<td>Certification scenarios and logs</td>
<td>REQUIRED</td>
<td>PLAZA fake fixtures outside cert/scenarios</td>
<td>Existing cert/scenarios contains TWIME only; ATEST covers local PLAZA</td>
<td>Scoped T99/TORDER only</td>
<td>BLOCKED</td>
<td>Add versioned PLAZA scenario manifests with exact evidence mapping</td>
</tr>
<tr>
<td>P2-EV02 / PLAN</td>
<td>Scheme/commit/config/runtime freeze</td>
<td>REQUIRED</td>
<td>Partial historical locks</td>
<td>Audit records current base; no final freeze</td>
<td>None for new full-log declaration</td>
<td>NOT RUN</td>
<td>Archive exact tested build, scheme fingerprint, config and benchmark host</td>
</tr>
<tr>
<td>P2-EV03 / PLAN</td>
<td>Replay recovery oracle across interruptions</td>
<td>REQUIRED</td>
<td>No public L3 harness</td>
<td>Add snapshot+partial+restart vs uninterrupted hashes</td>
<td>None</td>
<td>BLOCKED</td>
<td>Interrupt at receive/decode/apply/commit/checkpoint; exact state equality</td>
</tr>
</table>

No certificate or full-readiness claim follows from this matrix. Phase A stops for review as required by the attached plan.
