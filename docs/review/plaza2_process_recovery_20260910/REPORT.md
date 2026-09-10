# CGate connection-process recovery classification

Base: PR #52, `a6ede4cdc9aea4685a2de7d890606ad17f403470`. This is a separate stacked draft.

## Failure addressed

The immutable T1 `ZERO_ORDER_ROUTER_RECOVERY_FAIL` on source
`ca48ae94529f27926c3dffcef9ed68d96a5afaa9` remains FAIL. Its vendor log records
`MQ:SOCK_CLOSED` (24593 / 0x6011), socket closed by peer, and ACTIVE -> ERROR.
The process call returned `CG_ERR_INTERNAL` (131072), with no callback error. The old classifier
required AdapterState, so it made zero recovery attempts and the connector exited 3.
The restored router is not evidence of automatic connector recovery.

## Exact rule

The single-owner TEST host records failure origin structurally. It never parses an error message to
select recovery. Only direct connection-process **raw INTERNAL (131072)** is eligible for
bounded full rebootstrap, subject to callback/schema/decode/state-query fatal precedence.
Raw INVALIDARGUMENT, UNSUPPORTED, MORE, INCORRECTSTATE and unknown results are fatal.
OK and TIMEOUT are explicit successful outcomes at `Plaza2Connection::process`; the raw result
is retained. The translator already treated process TIMEOUT as success at the original reviewed
head; this correction makes that existing contract explicit at the runtime boundary. Other APIs'
timeout semantics are unchanged.

Bootstrap origins distinguish environment open, connection create/open, listener create/open and
publisher create/open. Only ConnectionOpen + raw INTERNAL can retry inside an established recovery
episode. Initial-start ConnectionOpen INTERNAL and all other bootstrap INTERNAL failures remain
fatal. The original deadline and first process cause survive failed reconnect attempts.

The first state API call after process failure is cg_conn_getstate, before observers or resource
teardown. State sampling retains the first failing state-query cause and corresponding origin rather
than silently converting an API failure into a transport transition. Existing uncreated optional
handles during startup are not misreported as vendor state-query failures.

Retained callback/decoder/private-bridge errors take precedence. An explicit callback cause plus a
process INTERNAL plus connection ERROR is fatal. Any failed state query is also fatal, including a
process INTERNAL followed by getstate INTERNAL; both causes remain available. Generic INTERNAL from
state, publisher, setup or unknown operations is not made recoverable.

For a healthy callback path, a direct process INTERNAL with connection ERROR enters Recovering.
The documented process lifecycle rule does not require the sampled state to have reached ERROR:
a direct process INTERNAL with ACTIVE is also covered, subject to the same exclusions and deadline.

The host retains the original process cause, first recovery-episode cause, current/final cause,
state-query cause, origin and captured transport health. Qualification JSON exports these separately.
A later failure episode replaces the first-cause binding; retries within an episode preserve it.
Original CGate codes/messages are never rewritten to imply success.

## Existing recovery and safety semantics

The patch reuses the existing close/destroy and startup paths. Readiness is invalidated before
teardown; retries wait at least one second and share the existing recovery deadline. Repeated process
failures after reopen do not extend that deadline. Fresh connection -> POS snapshot/anchor -> TRADE
replay -> remaining replication/AGGR -> publisher/reply dependencies remain unchanged.

There are no replstate shortcuts, order retries, automatic Cancel, cleanup or flatten commands.
POST_RECOVERY_OPERATOR_CANCEL_NOT_IMPLEMENTED remains unchanged. Existing Working-order,
post-invocation-before-reply and post-Cancel-loss tests now also exercise process INTERNAL.

No AGGR indexing, private-map delta staging, observer redesign, snapshot caching or ORDLOG/L3 work
is included. No qualification VPS load test or performance claim is made.

## Offline cases

- Realistic socket-close: fake cg_conn_process returns INTERNAL and changes ACTIVE to ERROR;
  Recovering closes every effective gate, delayed retry performs a fresh bootstrap, generation
  advances, replacement POS anchor/replay is selected, AGGR/publisher/reply recover, posts remain zero.
- Process INTERNAL with ACTIVE follows the same bounded lifecycle rule.
- Callback and decoder corruption surface through process INTERNAL plus connection ERROR: fatal,
  zero recovery attempts.
- Connection/listener/publisher getstate INTERNAL and process invalid argument: fatal, zero attempts.
- Process INTERNAL followed by connection getstate INTERNAL: both causes retained, fatal.
- Router remains down for two ConnectionOpen INTERNAL attempts: third attempt succeeds, or the
  original deadline expires with distinct process and reconnect causes retained.
- 1,000 forced TIMEOUT polls each before ONLINE, while Ready and after recovery: no recreation,
  generation change, posts or readiness invalidation merely from TIMEOUT.
- Initial bootstrap INTERNAL and non-ConnectionOpen recovery bootstrap INTERNAL remain fatal.
- Persistent process INTERNAL across successful reopens: deadline exhaustion, no busy loop or posts.
- Existing connection/listener/publisher/reply loss, order uncertainty, shutdown/signal, account identity
  and session-terms regressions remain part of validation.

## Qualification matrix and live gate

| Claim | Status |
|---|---|
| MOEX formula authority: upper = settlement + up, lower = settlement - down | PASS (user-relayed support correction) |
| PR #52 offline formula implementation | PASS |
| Historical 2077 / 184 / 184 -> 1893–2261 regression | PASS |
| New FutureSessionTerms implementation on T1 | NOT YET RUN |
| Previous live zero-order router scenario | FAIL, preserved |
| New classification on T1 | NOT YET RUN |
| Live order authority | NOT AUTHORIZED |

The next live scenario requires review of this PR and the exact source/binary before preparation
and launch, as required by section 15 of the continuation instructions. No automatic review label
or passing fake test substitutes for that gate. Do not bypass the date guard. If September 10's
window expires, prepare a truthful new dated harness. The corrected protected account must freshly
match exactly one PART row and fresh account exposure must be flat again before the live fault.
Use fresh evidence/journal directories, the dedicated router with no unrelated clients, zero posts
and no active epoch. Stop it once, do not signal the connector, restore it once, and require same-PID
fresh recovery before graceful shutdown. Preserve any failure and stop; do not patch and repeat live.

## Original reviewed-head validation receipts

These receipts describe `a6ba3f8`; corrected-head validation is reported separately.

Local macOS Release: **179/179**. Local ASan+UBSan component/PLAZA suite: **126/126**.
The old classifier was temporarily restored in this isolated worktree as a negative control: the new
socket-close regression failed as expected. Restoring the patch made the same test pass. The temporary
change was not committed. The observer-enabled one-shot getstate INTERNAL regression also passes.
Linux Release (including the normally excluded preflight label) and Linux ASan/UBSan/LeakSanitizer
are required through CI for the draft head. No passing offline result is a new T1 result.
