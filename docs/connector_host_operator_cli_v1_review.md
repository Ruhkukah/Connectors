# ConnectorHost and moexctl V1

Base: merged PR #33, `main@31f7b93b56a4c57bd820371708681bd41ae1df8d`.
Branch: `codex/connector-host-operator-cli-v1`.

## Boundary

`ConnectorHost` is the single-threaded orchestration owner of one existing
`Plaza2TestTradeTransport` and its existing session host. The 60-line CLI uses
only this facade and its operator configuration/rendering helpers. It does
not construct a publisher, projector, transport, or lifecycle controller.
There are no raw-post, add-only, cancel-only, or recovery-only host/CLI APIs.

The host exposes start/poll/stop, a value snapshot, current canonical plan,
exact-byte-and-SHA authorization, one-shot complete lifecycle submission,
and existing restart reconciliation. It does not reimplement the lifecycle.
Double start and poll-before-start fail; stop is idempotent. Readiness is
recomputed, not latched across stale data. A stopped host cannot be restarted.
The lifecycle operation is synchronous; callers must serialize access.

The only transport changes provide read-only evidence access. Existing
position/provenance assessments are reused. The optional observation client
context handles BF account type and short client-code UOB rows before an
intent is installed; default arguments preserve existing send-path behavior.
Replay readiness is reported independently even when an exact POS row caused
position classification to return early. The inspection result is unpersisted,
has no canonical bytes/SHA, and is not an execution-safety receipt or authority.

Codec, publisher, AGGR projection, private-state projection, lifecycle,
C ABI and managed ABI implementations are unchanged. No TWIME integration,
production enablement, router changes, exchange connection, or live order was
performed. Standalone AGGR LifeNum and ReplyBridge FORTS_MSG99/100 remain deferred.

## Safety and operator contract

`status` and `qualify` use `LiveTestPreSend`; they reject the send arm and
authorization inputs. A qualification-purpose native host also refuses
authorization/submission, including with a fake runtime. They never call post.

`order-test` requires all four explicit TEST arms and an exact canonical plan
file plus SHA. Static fields supplied on the command line must reproduce those
bytes through the existing plan builder. There is no automatic authorization,
interactive yes/no, repricing, or fallback to another target. After authorization,
the host polls and rechecks current readiness, then delegates the complete
Add/observe/cancel-or-recover/terminal lifecycle to the existing controller.
The transport retains its independent preflight, receipt-before-send, one-Add,
exact payload binding, ordinary reply/TRADE identity checks, and lock policy.

Readiness requires current private/status streams, UOB periodic consistency,
three same-LifeNum target provenance records, exact session membership,
status=1, fresh uncrossed two-sided AGGR20, unique applicable PART limits,
zero position evidence, exact POS-anchored current TRADE, and zero active own
UOB orders. UOB and TRADE are never reconciled against each other.

`market_safe` means **lifecycle terminal safety**, not permission to submit.
It is false before a completed lifecycle. `observation_ready` is the separate
dynamic readiness result. Numeric quantities, IDs, anchor revisions and row
provenance are serialized without floating-point conversion. Snapshots contain
no credentials, software key, raw account codes, connection URLs or auth data.
Runtime failures expose numeric codes rather than potentially secret settings.

## Usage

Build `moexctl`; see `build/apps/moexctl --help` for all explicit options.
Run from a dedicated writable CGate client state/log directory, not a shared
router directory. CLI V1 does not change cwd or installation permissions.
Use absolute runtime/scheme/config/receipt/journal paths.

For read-only status/qualification, provide runtime root, scheme/config dirs,
current exact isin/session IDs and the three connection arms. Endpoint is
fixed to local T1 `127.0.0.1:4101`. Publisher name is unique per invocation and
the reply ref is constructed from that same name. The five private streams,
two status streams and AGGR20 use the existing reviewed stream semantics.

`--env-settings-var` names an environment variable containing the CGate env
open settings (including the secret key); `--broker-code-env` and
`--client-code-env` name account variables. Credentials/software-key providers
also use named environment variables. Do not put secret values in argv.
No new configuration language or generic profile format was introduced.

For order-test, also supply canonical plan file/SHA, price/side/base contract,
optional original comment, exact ext/user IDs, profile/policy identity,
run ID, journal and execution receipt paths. Quantity is fixed at 1 LIMIT,
distance at 4 ticks, freshness at 5000ms, and zero position is required.
Canonical files from the existing builder must not be reformatted. Native
clients can call `plan()` to prepare an exact current plan for review.

Output is human-readable by default or `--json` (`moex.connector-host.v1`).
Readiness waits are bounded by `--wait-ms` (default 10000, maximum 60000).
Exit codes: 0 completed readiness/lifecycle; 2 invalid arguments; 3 startup;
4 not ready; 5 authorization refusal; 6 unsafe/failed lifecycle; 7 close failure.
Always inspect the lifecycle state and counters: safe `DefinitelyNotSent` is
not proof of an exchange-accepted order. Qualification output is captured
before explicit close, preserving the state actually observed.

## Validation

All tests use the instrumented fake CGate library. No vendor runtime was loaded.

| Check | Result |
| --- | --- |
| Full CTest, including existing ABI/no-send/privacy/style tests | 160/160 passed |
| Changed-target ASan/UBSan: host, CLI, transport and runner harness | 10/10 passed |
| Repository/source style and diff checks | Passed |
| GitHub connector-validation and component-sanitizers | Required before merge |

Host tests cover start/ready/stop, invalid sequencing, typed row provenance,
immutable anchor coherence, periodic UOB, exact-zero and sparse-POS evidence,
negative readiness, no-send qualification, exact authorization, normal cancel,
fill without compensating order, uncertain Add recovery without retry, and
identity-conflict unresolved terminal with all four identifier locks retained.

CLI tests execute the real binary through the host and fake runtime: help,
invalid args, missing arms, JSON schema/readiness/provenance, qualification
zero allocations/posts, crossed-book refusal, and a complete fake Add/cancel
with exactly two allocations/posts and a persisted execution-safety receipt.
The sanitizer CI target list includes both new binaries. Local macOS sanitizer
execution disables unsupported leak detection; CI retains Linux leak detection.

### CI harness amendment

The initial head's Linux connector-validation job passed. Both sanitizer
attempts passed the host/CLI/PLAZA cases but failed the existing TWIME runner
test's expected-state wait. Investigation found a test scheduler issue:
256 non-blocking polls advanced a manual clock without yielding to the real
TCP server thread. A deliberate 20ms ACK delay reproduced the same failure
locally. A 1ms yield in the test pump made that delayed-reply case pass ten
consecutive runs. The original poll budget, virtual-clock advancement,
protocol assertions and runtime deadlines remain unchanged. Only the test
helper and delayed-reply regression changed; there is no TWIME runtime or
host integration change. Full CTest and changed-target sanitizers were rerun.

C ABI v2 is the separate next increment after this PR merges. It must use this
same facade and retain existing sized-layout, event and managed ownership work.
