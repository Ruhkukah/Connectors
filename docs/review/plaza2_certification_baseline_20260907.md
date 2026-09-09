# PLAZA II certification baseline — Phase A, 2026-09-07

Phase A is a documentation-only audit of current remote main. **Not certification-ready.** The audit is complete enough to identify the implementation sequence; the current
official wire freeze and exact snapshot/open semantics remain explicit blockers. The attached plan section 34 requires review here before implementation. No production behavior
changed, no T1 connection was made, no exchange order was transmitted, and no PR was merged.

## Repository and environment

| Item | Observed value |
| --- | --- |
| Remote | `https://github.com/Ruhkukah/Connectors.git` |
| Current remote main / audit base | `22a9dfd0947ccde4645696974e1270099491e2c6` (fetched 2026-09-07) |
| Original local main | `ad100ae646d47ce4befc0314c62b79e9d2f08742` |
| Audit checkout | `/Users/pavel/CSharp/MoexConnector-cert-a` |
| Audit branch | `codex/plaza2-certification-phase-a-20260907` |
| Original tracked changes | None |
| Original untracked paths | `.codex-tmp/`, `build-cabi-v2/`, `build-cabi-v2-asan/`, `build-pr36/`, `build-pr36-asan/`, `build-pr36-dotnet/`, `docs/evidence/plaza2_pr30_send_disabled_20260906.zip`; preserved |
| OS / architecture | macOS Darwin 24.6.0, arm64 |
| Compiler / language | Apple Clang 17.0.0, C++20; this is not a Rust repository |
| Build tooling | CMake 4.2.3, Ninja; Release build |
| Test tooling | Python 3.14.3 with isolated PyYAML 6.0.3; .NET 10.0.201 |
| Target (existing recorded qualification) | SPECTRA9.9.0, DDS 990.1.6.42744, CGate package `cgate_9.9.0-2008_amd64.deb`, library `6.102.0.6118`, router `229.123.0.7233` |
| Scope exclusions | TWIME/FIX/FAST/SIMBA/ASTS, integrations, strategies, V1/V2 redesign, unrelated cleanup |

Only one open PR was returned by GitHub: draft [#24](https://github.com/Ruhkukah/Connectors/pull/24), “Implement phase 5E PLAZA II TEST transactional order-entry bring-up”, head
`24e4619e29244a6c11f368044550aabf04653a86`, base main. It overlaps runtime, live session, transactional runner, tests and VPS scripts. It was inspected, not merged. No open ABI PR
was returned. V3 is already merged in #37, so the historical “draft V3” assumption is obsolete.

Relevant merged increments include #28 connectivity, #29 periodic USERORDERBOOK readiness, #31 CGate 9.9 parity, #32 reference-row provenance, #30 live pre-send parity, #33 TEST
submission, #34 operator CLI, #35 C ABI V2, #36 persistent host epochs, and #37 persistent C ABI V3. PR numbers are inventory, not implementation authority.

## Control documents and evidence levels

- [Certification matrix](../../cert/PLAZA2_CERT_MATRIX.md): official Appendix 1 items plus plan-specific refinements, one row per requirement.
- [Exact public schema inventory](plaza2_ordlog_schema_audit_20260907.md): every field of all ten tables, repository IDs, generated-type limitations.
- [Machine-readable audit receipt](plaza2_certification_audit_20260907.json): source fingerprints, field comparisons, test inventory and failed-run classifications.

`PASS` in the matrix applies only to the evidence scope specified. Existing T1 evidence is historical and binds its original SHA, not this audit's HEAD. A local fake-runtime pass
does not prove MOEX infrastructure behavior. `BLOCKED` identifies absent implementation/protocol authority; `NOT RUN` identifies missing qualification. No public-order-log
requirement is N/A.

## Official material checked

1. [MOEX Customer Software Certification Procedure](https://www.moex.com/files/4qg0gqtzcxkep68687ah1bwq5e), approved 2021-09-23, retrieved with web tooling in this audit. General
sections 2–3 and Appendix 1, printed pages 5–7, are the matrix's external anchors. Appendix 1 receiving replicas item 7 explicitly sets **100,000 messages/s** for full ORDERS_LOG.
Sending commands items 3 and 6 explicitly require configurable rate control and handling replies 99/100. The current additional inquiry form and any superseding MOEX requirements
still need to be bound at qualification.
2. [MOEX Plaza II product page](https://www.moex.com/s444) and [connection requirements](https://www.moex.com/a584), retrieved now. These do not establish a 9.9 binary contract.
3. Local cached manual `/Users/pavel/CSharp/MoexConnector/.codex-tmp/cgate99_installed_p2gate_en.html`, title “PLAZA II gateway (version 9.9)”, SHA256
`5e2bfa2f6e8b3bc48f72e2eb46230e61200fde2a4bb5dbf723eabc28f1193c2e`, read and compared now. This untracked cache is **supporting evidence**, not a freshly authenticated official
distribution. The receipt preserves the extracted field comparisons, not the full vendor manual.
4. Existing committed 9.9 runtime lock: `spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/manifest.yaml`; official scheme recorded hash
`7b93117ee435fd0cb2849b677fc32a9d581364b6ee9afeac9c6c002875400746`. Raw vendor files are intentionally not committed. Runtime signature reports alone do not expose exact new wire
offsets and nullability.

Fresh access to `https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/` and `.../test/Scheme/` failed: web fetches returned timeout/502; local HTTPS clients failed
issuer-certificate validation on both .com and .ru. Validation was not disabled. Publicly indexed 9.3 documentation is not substituted for 9.9. Therefore **current official
ORDLOG/ORDBOOK byte-for-byte verification is BLOCKED**, despite the local manual field match. Phase B must acquire and fingerprint the actual official 9.9 scheme/header or an
authenticated copy of the already locked distribution.

## Existing implementation and qualification gaps

<table>
<tr>
<th>Capability</th>
<th>Implemented / locally tested</th>
<th>T1 proof / sufficiency</th>
<th>Remaining work</th>
</tr>
<tr>
<td>Runtime and typed listeners</td>
<td>Runtime, callback, scheme validation, live runner and tests exist</td>
<td>Eight streams + open-only publisher in 9.9 parity receipt; sufficient for that bring-up only</td>
<td>Exact certification URLs/thread ownership, idle, outages, failover and full-day evidence</td>
</tr>
<tr>
<td>Private replication</td>
<td>Transaction visibility, scoped LifeNum, ClearDeleted, POS anchor and periodic UOB readiness tested</td>
<td>Existing 9.9 TRADE/UOB/POS/PART/REFDATA/status stream receipt</td>
<td>Full recovery matrix; no general durable replication+state checkpoint store</td>
</tr>
<tr>
<td>Listener reopening</td>
<td>Initial-open ERROR retry tested</td>
<td>No full outage qualification located</td>
<td><code>supervise_initial_listener_opens()</code> explicitly rejects ERROR after first completed snapshot; runtime reconnect coverage is incomplete</td>
</tr>
<tr>
<td>AGGR20</td>
<td>Independent projector, exact scaled price, per-instrument freshness and tests</td>
<td>Live AGGR20 receipt and bounded order-run BBO evidence</td>
<td>Standalone bridge ignores LifeNum; both bridges reset entire book on ClearDeleted; table/revision semantics and recovery need qualification/fix</td>
</tr>
<tr>
<td>Add/Cancel</td>
<td>Guarded live transport, correlated reply, private replication terminal proof; regression suite passes</td>
<td>2026-09-06 one Add then one DelOrder, source <code>ffc5df7f00000f1babc2a2df9cc5d9c950cdb5c1</code></td>
<td>Preserve; broader commands and certification-day run remain unqualified</td>
</tr>
<tr>
<td>Other commands</td>
<td>MoveOrder, DelUserOrders, CODHeartbeat codecs/fakes exist; exact-ext recovery is implemented</td>
<td>No general live qualification for all codec commands</td>
<td>Freeze actual declaration; codec support is not a live capability claim</td>
</tr>
<tr>
<td>Replies 99/100</td>
<td>Codec + official-layout decode tests exist; live path still rejects families</td>
<td>No end-to-end or T1 acceptance evidence</td>
<td>Narrow live bridge + observation whitelist fix, correlated/unmatched and duplicate tests</td>
</tr>
<tr>
<td>Publisher rate control</td>
<td>No configurable gate found in PLAZA transport/config</td>
<td>None</td>
<td>Dedicated small gate immediately before sending; explicit accounting and reconnect/timeout tests</td>
</tr>
<tr>
<td>Publisher reconnect</td>
<td>RAII open/close and timeout certainty exist</td>
<td>Open-only / one lifecycle evidence</td>
<td>No supervised error/reopen implementation located; booleans indicate successful open, not continuous publisher state</td>
</tr>
<tr>
<td>ORDLOG and ORDBOOK</td>
<td>Metadata for 4 + 6 tables; generic runtime can resolve descriptors</td>
<td>No full public pipeline proof</td>
<td>Wire freeze, lossless callback, raw listener, snapshot/log coordinator, L3, recovery, batch API</td>
</tr>
<tr>
<td>Full-log throughput</td>
<td>No end-to-end ORDLOG benchmark</td>
<td>None</td>
<td>Required 100k gate, internal 200k target/300k burst, slow consumer and coexistence</td>
</tr>
<tr>
<td>Evidence</td>
<td>Receipts, journal, redaction, runners exist</td>
<td>Historical scoped bundles only</td>
<td>Versioned scenario corpus, complete official item mapping, coordinated runs and freeze</td>
</tr>
</table>

### Reply 99/100 finding

`connectors/plaza2_trade/src/plaza2_trade_codec.cpp` decodes `FORTS_MSG99` (queue size, penalty remaining, c128 text) and `FORTS_MSG100`; tests live in
`tests/plaza2_trade/plaza2_trade_reply_decoding_test.cpp` and `fixtures/cgate99_messages.hpp`.

But `ReplyBridge::on_plaza2_listener_event()` in `plaza2_test_trade_transport.cpp:421` permits only command-specific families 179/177/186 and returns `CallbackFailed` for 99/100.
The observation loop near line 2129 has a second whitelist. A legitimate reply therefore never reaches the available decoder. This is a verified software gap, not a claim that an
observed production hang occurred. It fails closed; the code review and existing uncertainty tests do not show blind retransmission. Fix classification/correlation and deadline
completion, preserving PossiblySent reconciliation and no duplicate Add.

### Rate-control finding

Search of PLAZA runtime, transport, configs, and tests found no transaction rate configuration or pre-send rate gate. `penalty_remain` is decoded but not used for scheduling.
Observation deadlines and one-order authorization are not rate controls. Phase F should add one single-owner monotonic-time gate shared across authorized publisher commands,
without sleeping in the CGate poll loop. Specify attempted-post accounting (including ambiguous posts), no refund based on a reply timeout, retry eligibility only for
definitely-not-sent commands, preserved budget across reopen, and configured limits tied to provisioned access. Dynamic limit adjustment is currently unsupported; do not invent
automatic discovery. Exact penalty units and treatment must be verified before implementation.

### AGGR20 findings that must not be hidden by green tests

The standalone bridge in `protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp:244` ignores LifeNum and leaves projector state intact. The transactional `AggrProjectorBridge` resets
state on LifeNum/Close. Both reset the whole projector on ClearDeleted. This is inconsistent behavior and does not prove the required table/revision-scoped deletion semantics. Test
each entry point and an active row surviving an unrelated deletion boundary. Keep this remediation narrow; do not derive AGGR20 from ORDLOG.

## Replication reuse map

<table>
<tr>
<th>Concern</th>
<th>Reuse exact implementation</th>
<th>Necessary extension / boundary</th>
</tr>
<tr>
<td>Environment, connection, listener, publisher</td>
<td><code>plaza2_runtime.hpp/.cpp</code>: <code>Plaza2Env</code>, <code>Plaza2Connection</code>, <code>Plaza2Listener</code>, <code>Plaza2Publisher</code></td>
<td>Verify actual public stream settings/indices; retain one owner thread and existing RAII</td>
</tr>
<tr>
<td>Callback and scheme</td>
<td><code>listener_callback_bridge</code>, <code>ensure_listener_scheme_loaded</code>, <code>RuntimeMessagePlan</code> in <code>plaza2_runtime.cpp</code></td>
<td>Opt-in raw replication span, null map, message index and exact time/decimal access; keep current consumers compatible</td>
</tr>
<tr>
<td>Event boundary</td>
<td><code>Plaza2ListenerEvent</code>, <code>Plaza2ListenerEventHandler</code></td>
<td>Callback-borrowed bytes only until return; queued batches must own bytes/null map</td>
</tr>
<tr>
<td>Transaction staging</td>
<td><code>Plaza2PrivateStateBridge::handle_stream_data</code>, transaction begin/commit, private projector visibility rules</td>
<td>Reuse ordering semantics; do not route 100k records through fake <code>FieldValueSpec</code> conversions or copy whole state per transaction</td>
</tr>
<tr>
<td>LifeNum</td>
<td><code>handle_lifenum</code>, per-stream <code>stream_lifenums_</code>, private invalidation tests</td>
<td>Public generation reset coordinated with snapshot&#x27;s ORDLOG life, not a global equality test across unrelated streams</td>
</tr>
<tr>
<td>ClearDeleted</td>
<td>Runtime decodes table index, revision and flags; bridge queues controls to transaction application</td>
<td>Public replicated-row retention distinct from active orders; do not reuse blanket clearing; audit exact official threshold/flags</td>
</tr>
<tr>
<td>Replstate</td>
<td><code>handle_replstate</code>, <code>ResumeMarkersSnapshot</code>, fake <code>resume_from_replstate.yaml</code></td>
<td>Existing marker is in memory and does not provide durable public projector+checkpoint atomicity; need per-stream committed boundary</td>
</tr>
<tr>
<td>Health</td>
<td><code>StreamHealthSnapshot</code>, <code>Plaza2LiveHealthSnapshot</code>, <code>HealthTrackingHandler</code></td>
<td>Online currently marks snapshot complete; L3 readiness requires validated boundary, applied log and zero backlog</td>
</tr>
<tr>
<td>Reopen</td>
<td><code>Plaza2TestSessionHost::Impl::supervise_initial_listener_opens()</code></td>
<td>Extend bounded supervision for mid-run public recovery; existing private hot restart is not already solved</td>
</tr>
<tr>
<td>Scheme workflow</td>
<td><code>plaza2_schema_materialize.py</code>, <code>plaza2_codegen.py</code>, <code>plaza2_runtime_scheme_lock.py</code></td>
<td>Promote public table profile to required, freeze byte layouts, preserve additive ABI IDs</td>
</tr>
<tr>
<td>Restart/evidence</td>
<td><code>plaza2_order_lifecycle.cpp</code>, ConnectorHost persistent epoch checkpoint/journals, transport execution-safety receipts</td>
<td>Reuse file/evidence conventions only; order epoch checkpoint is not a durable L3 checkpoint</td>
</tr>
</table>

The leanest initial restart policy is a **fresh snapshot**, explicitly discarding public in-memory state and stale markers. Persisted replstate alone must never resume into an
empty L3 map. Later durable resume requires a single manifest binding state hash, generation, schema, and per-table applied boundaries; publish only after transaction application
and mandatory consumption succeed. No new generic recovery framework is warranted.

## Proposed C++ design (not implemented)

Use two native module pairs first: `plaza2_ordlog.hpp/.cpp` for raw ingress, bounded batches and stream status; `plaza2_l3_book.hpp/.cpp` for snapshot coordination and one-owner
order projection. Reuse runtime and common listener controls. Keep metrics in these owners until a real reason to split them appears. One replay executable can serve correctness
and performance modes. No Rust, new scheduler or speculative worker sharding.

```cpp
// Logical candidates, NOT frozen packed ABI or generated wire structs.
struct Plaza2OrdlogEnvelope {
    std::uint64_t receive_sequence, generation, life_num;
    std::uint64_t receive_monotonic_ns;
    generated::StreamCode stream;
    generated::TableCode table;
    std::uint32_t message_index;
    std::int64_t repl_id, repl_rev, repl_act;
};
struct Plaza2OrdlogRecord {
    std::int64_t public_order_id, public_amount, public_amount_rest;
    std::int64_t id_deal, xstatus, xstatus2;
    std::int32_t sess_id, isin_id;
    std::int64_t price_scaled_1e5, deal_price_scaled_1e5;
    std::uint64_t moment_ns; // absolute exchange epoch ns
    std::int8_t dir, public_action; // unknown values preserved
};
struct Plaza2OrdlogEventView {
    Plaza2OrdlogEnvelope envelope;
    std::span<const std::byte> wire_bytes;
    std::span<const std::byte> null_map;
};
enum class Plaza2L3State : std::uint8_t {
    Disabled, Connecting, Snapshotting, CatchingUp, Ready,
    Stale, NeedsResync, Recovering, Failed
};
```

The record above is a convenience view; losslessness resides in exact retained wire bytes/null map plus the schema fingerprint and envelope. All ten table variants must be
delivered, including heartbeat/system events and both multileg prices. `moment` must remain separately available from generated wire access even when `moment_ns` exists; no
conversion to seconds or inference that one substitutes for the other. Generated typed accessors/layout validation will be derived from the official scheme, not this sketch.
Non-null prices use exact checked integer scaling, never double; nulls remain null.

One active-order index is sufficient initially; sorted depth and canonical hash are computed on demand. Candidate identity is `(matching partition, generation, sess_id, isin_id,
public_order_id)` with documented table-kind separation if necessary. **Key is not frozen**: verify uniqueness and multi-day rollover continuity before C2; the manual's iceberg
identity description is insufficient to prove global uniqueness. Never associate this key with private orders.

Apply documented actions 0/1/2 only after checking preconditions; snapshot `public_amount_rest` seeds active remainder without replaying the snapshot row as a historical execution.
Unknown actions stay available raw and invalidate L3 until interpreted authoritatively. Duplicate suppression must validate the same generation/table/revision and exact payload;
conflicting duplicates cause resync. Do not assume adjacent order-log records must have consecutive numeric revisions; other replicated tables and filters may consume revisions.
Gap detection must use verified stream coverage/control semantics, not `rev == previous + 1`.

## Proposed snapshot/log recovery and protocol gates

The local 9.9 manual's “Handling abnormal situations / Recovery algorithm” recommends the anonymous snapshot followed by ORDLOG from the snapshot revision. “Stream
FORTS_ORDBOOK_REPL” describes periodic publication and `publication_state`; tables 40/43 identify `trades_rev` and `trades_lifenum`. “Single calendar day data / Scenario for
working with new data” illustrates CGate-managed `p2ordbook` handoff. [The official indexed API reference](https://ftp.moex.ru/pub/ClientsAPI/Spectra/Docs/cgate_en.pdf) documents
`snapshot.bind`, but is not a verified matching 9.9 SDK.

There are two concrete documentation conflicts: the prose example still uses `info_currentday.logRev`, while table 43 uses `trades_rev`; the recovery table misspells the public
snapshot stream `FORTS_ORDRBOOK_REPL`. Use neither string as unquestioned implementation authority.

**Preferred candidate:** one CGate-managed `p2ordbook` bootstrap per required table pairing, using the actual public ORDLOG/ORDBOOK streams and verified binding, with an
independent lossless raw ORDLOG product where composite snapshot delivery would otherwise hide history. Avoid two parallel ingestion paths if verified composite semantics can
provide both products correctly. Confirm public pairing, multileg mapping, synthetic message layouts, LifeNum behavior, and how publication completion is honored in the actual 9.9
library first.

**Alternative documented shape:** stage a complete periodic ORDBOOK publication, read its committed ORDLOG boundary `(life, R)`, then open ORDLOG using that exact boundary and
process retained updates after R. It avoids the wall-clock race because updates generated during snapshot acquisition are replayed from the exchange revision, not from connection
time. This is valid only after verifying boundary inclusivity, retention/rejection behavior and full multileg coverage. A finite local buffer is not a proof of coverage if history
before subscription is missing.

Proposed lifecycle, contingent on resolving those points:

1. Startup: mark Connecting/Snapshotting, stage one publication; partial publications never replace the visible generation. Require matching metadata and completed publication, not merely listener ONLINE.
2. Handoff: select immutable `(ORDLOG life, R)` at committed completion; obtain all required table history after the boundary via verified CGate semantics. Fail if snapshot changes
mid-acquisition or coverage is unavailable.
3. Catch-up: apply transactionally and validate each operation. Ready requires completed consistent snapshot, current life, verified coverage, healthy projector, committed
increments and zero local backlog after online catch-up. Heartbeat alone does not prove completeness.
4. Restart: initially choose fresh bootstrap. A future verified durable state+checkpoint pair may resume; missing state, wrong scheme/hash, stale checkpoint or rejected history must bootstrap afresh.
5. LifeNum: invalidate the entire L3 generation, expire borrowed views, discard staged state and unsafe checkpoints; reacquire a snapshot tied to the new log life.
6. Gap, contradictory duplicate/action or queue overflow on the mandatory path: NeedsResync, no checkpoint advance, no Ready book; restart bootstrap under bounded retries.
7. Snapshot failure/close or publication never completing: discard staging, report reason, bounded delayed reopen; terminal repeated errors become Failed.
8. ClearDeleted: retain active-order semantics independently of expired replicated history. If a required reconstruction boundary becomes unavailable, resync. The exact table range
predicate and flags remain a Phase B/C2 verification item.

C2 cannot claim a race-free implementation until the 9.9 binding/retention/transaction rules are resolved with fixtures or official samples. This is an identified protocol gate, not permission to invent a coordinator.

## Performance and ownership contract

Start with the connection's existing single owner thread: byte validation/decode, raw mandatory delivery, L3 mutation, commit accounting and cheap counters. The current generic
runtime's string formatting and fake-field bridge are unsuitable as the only full-log path. Add an opt-in typed/raw path behind the existing runtime boundary; preserve legacy
consumers. Precompute field plans, reserve bounded batch storage and an order index. Adds may allocate an order-map node; cancel/trade updates should avoid allocation. Enforce
maximum active orders, pending transaction size and snapshot staging size; exhaustion must fail visibly, not grow without bound. No whole-book copy on each commit;
batch/touched-order staging must preserve failed-transaction invisibility.

Optional raw subscribers receive bounded owned batches; overflow records the last delivered/first missing sequence and invalidates that subscription. It must resubscribe/replay
rather than appear lossless. Mandatory overload invalidates connector health and cannot advance its checkpoint. No per-record JSON, formatted logs, fsync, managed callback or
global contended lock. Sample timing/latency without a per-record OS syscall; caller-provided receive timestamps may be batch-scoped and must be labelled accordingly. Exact receive
sequence remains per record.

Proposed Phase D gate, to freeze before measurement: Release build on a named Linux x86-64 qualification machine, 60 s warmup then 10 minutes independently paced at 100,000 wire
records/s; measure source scheduling lateness so a stalled producer cannot fake zero backlog. Report one-second arrival/processing/backlog/oldest-age series, queue high-water,
drop/gap/resync counts, latency percentiles, RSS, final canonical full-state hash and all build/corpus fingerprints. Pass only with all input accounted for, correct final hash,
zero silent loss/unexplained gaps/resync, no sustained backlog/oldest-age growth and bounded memory. Reject a run whose offered load is below target. A fast unpaced throughput loop
alone cannot pass this gate. Proposed engineering runs: 200k sustained; 300k bursts of 10 s with drainage within the following 10 s at 100k; label these internal targets.

Use deterministic byte fixtures with adds/cancels/partial and full execution, repeated and conflicting records, both table families, many instruments, deep books, session turnover,
LifeNum and ClearDeleted. Include restart at every critical transaction/checkpoint boundary. Compare the canonical sorted full order state and final checkpoint against
uninterrupted replay. Validate coexistence by injecting AGGR20/private callbacks and publisher/reply work into the same pump with fixed fixtures; an ORDLOG-only parse loop is not
concurrency qualification. Freeze benchmark parameters and finite queue/book capacities before collecting outcomes. No performance result is claimed in Phase A.

## Sequence and acceptance gates

| Increment | Bounded deliverable | Gate |
| --- | --- | --- |
| A | This baseline, matrix, schema inventory, evidence receipt | Review findings and unresolved official-source points; no functional changes |
| B | Official 9.9 wire freeze; reuse existing ten tables; generated access/layout fixtures; required public scheme profile | Every consumed public table verified; handoff semantics resolved |
| C1 | Raw ORDLOG listener, lossless bytes/fields/envelope, bounded delivery/status, lifecycle fixtures | Complete raw stream without L3 dependency |
| C2 | ORDBOOK bootstrap, deterministic single-owner L3 and recovery, generation invalidation/hash | Uninterrupted and supported interrupted paths produce identical state/checkpoint |
| D | One replay/qualification executable with performance modes | 100k real hot-path gate; separate engineering headroom, burst and coexistence reports |
| E | Additive batch/snapshot native/managed API with explicit lifetimes | No per-record cross-language call; V1/V2 compatibility preserved; V3 is already merged |
| F | Narrow fixes for 99/100, rate gate, publisher/mid-run listener supervision, AGGR controls, residual matrix gaps | Every offline applicable row passes with direct evidence |
| G | Guarded, coordinated T1 scenarios and full day; freeze exact software/config/runtime/corpus/evidence | No software blocker; unresolved coordinated items explicitly disclosed |

## Validation and reproducibility

Configured and built the exact audit base in `/tmp/plaza2-cert-a/build` with Release, Ninja, Apple Clang 17 and repository .NET tests enabled. Native build completed all 241
initial steps. Linker warned about duplicate static libraries; build succeeded. No application source was changed to make tests pass.

The first non-preflight run was **123 passed / 40 failed**: the selected Python lacked PyYAML, and external-build runner discovery also failed. Installed the repository's declared
dependency into `/tmp/plaza2-cert-a/venv` and configured `Python3_EXECUTABLE` there: **158 passed / 5 failed**. The remaining failures were TWIME cert-runner lookup at hardcoded
source `build/apps` paths; a PATH-only retry stayed 158/5. A mistaken invocation against the local generated output directory found zero tests and is explicitly excluded as
evidence. Added an untracked `build/apps` symlink to the actual external build's apps directory; final run **163 passed / 0 failed**, including **54 PLAZA-labelled tests**, in 6.31
s.

```sh
cmake -S . -B /tmp/plaza2-cert-a/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE=/tmp/plaza2-cert-a/venv/bin/python
cmake --build /tmp/plaza2-cert-a/build -j 8
# Existing runner discovery needs source build/apps to resolve to this build's apps.
PATH=/tmp/plaza2-cert-a/venv/bin:$PATH ctest --test-dir /tmp/plaza2-cert-a/build \
  --output-on-failure -LE preflight -j 8
```

After staging, two style checks caught overlong document/table/JSON lines; formatting was corrected without code changes.
Final staged-document preflight: **3 passed / 0 failed** (Unicode, repository style, source style). Together with the 163-test regression run, **166/166 registered tests passed**. Results and test-log
fingerprints are recorded in the receipt. Sanitizers: **NOT RUN** (documentation-only increment); CTest's `sanitizer` label does not mean this Release build was instrumented.
ORDLOG performance: **NOT RUN**, no implemented hot path. Cargo: not applicable to this C++ project. Optional AlorEngine shadow replay was not configured and is outside scope.
Historical CI/sanitizer results are not presented as newly reproduced.

Network access: GitHub inspection/fetch, MOEX public-document reads/download attempts, dependency download, and local fixture sockets. T1 accessed: **no**. Orders transmitted to
MOEX: **zero**; synthetic publisher fixtures exercised fake sends only.

Stop at the audit gate required by the attached plan. Next implementation is B, after review; unresolved current vendor wire layout and recovery semantics remain visible blockers rather than guessed code.
