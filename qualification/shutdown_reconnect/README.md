# Isolated shutdown/reconnect qualification harness

This is a separately linked TEST-only, no-post host harness. It does not install,
arm, monitor or start a job automatically. Building or passing fake-runtime tests
is not native qualification. The ordinary `moexctl` executable has neither of the
owned-connection injection methods nor the harness publisher override.

## Build and scope

Configure explicitly with `-DMOEX_BUILD_SHUTDOWN_HARNESS=ON` (default OFF).
The target is `moex_shutdown_reconnect_harness`, under `build/qualification/`.
It compiles the same host/session source into separate archives with
`MOEX_ISOLATED_SHUTDOWN_HARNESS`. That build rejects every publisher post before
command-message allocation, reply registration, rate admission or native posting.
Regression tests verify Add, Del, Move and mass cancel are definitely not sent.
The production archives and normal CLI retain their original publication path.

The harness closes only the CGate connection owned by its process. Ordinary
session recovery invalidates listeners and reconstructs history. A test-only hold
can prevent that owned connection from reopening; it does not interrupt the
router, upstream service, another client, host network or clocks. The normal host
shutdown cancellation logic remains unchanged. Kill switch stays on throughout.

## Future execution inputs (not execution authorization)

`--help` describes the required flags. Native execution additionally requires
`--execute-isolated-test`, one case, the approved TEST runtime/library/scheme,
credentials/software-key/login environment variable names, explicit broker/client,
positive ISIN, fixed quote cap, assigned ID range, journal and identity checkpoint.
Environment-open settings come from an environment variable, not a printed command.
Only the TEST profile and localhost TEST router `127.0.0.1:4101` are selectable.
The main program has no fake-runtime mode or PROD switch. The assigned range must
cover at least 7,000 IDs; the durable checkpoint must also leave at least 7,000 remaining IDs, checked before runtime start.
Existing durable identity reservation/locking is used without reset or rollback.
The host may reserve IDs during startup even though publication is prohibited.

Before any native invocation, the operator must verify the sealed source/binary
and vendor-library hashes, authorized date/session, sole ownership, old-process
termination, independent account flatness, assigned range/checkpoint and remaining
capacity. Use a fresh journal; preserve previous evidence. Do not share a product's
connection or checkpoint concurrently. Run sequentially and independently verify
zero clients after each process. This tool cannot recover account exposure.

## Cases and acceptance

Admission is bounded to 15 seconds and requires current complete private/status
streams, ACTIVE native transport/listeners/AGGR/reply, matching POS/TRADE epoch,
zero queued/working/pending orders, zero account positions and no host safety/storage
error. Complete empty POS is allowed; incomplete absence is not. Admission failure
performs no connection injection. Fresh reconstruction also precedes positive
shutdown completion. TRADE/USERORDERBOOK can complete before the reply, AGGR or status listeners; the harness waits for the full fresh-flat gate within the same ten-second budget. If that broader reconstruction never completes, it refuses positive acceptance.

- `healthy`: complete empty native state, production shutdown request, expected
  shutdown outcome 0; no connection interruption.
- `recovered-empty`: close the owned connection, poll its loss, request shutdown
  while reconstruction is unknown, verify deferred scopes without queued cancels,
  then permit normal reopen. Expected outcome 0 only after fresh reconstruction.
- `unavailable`: hold that owned reconnect beyond the production ten-second drain.
  Expected negative-control outcome 8, timed out and reconciliation pending.
- `second-loss`: recover the first loss, prove a fresh empty state, then close the
  owned connection again and hold it beyond the original ten-second deadline.
  Expected outcome 8; the previous proof must be invalidated.

The JSON receipt reports admission, observed loss/defer/recovery, timeout,
reconciliation/cancellation/working flags, stop result, modeled shutdown outcome,
source SHA and a static failure code. Positive cases exit 0, expected timeout cases
exit 8, input/admission/other harness failures exit 2. A nonzero stop produces
`shutdown_exit=7` and a failing receipt. Exit 8 in a negative control is expected;
it must not be described as successful execution closeout or flatness proof.
Raw native timelines stay in the private host journal; publish sanitized timestamps,
interval hashes and correlated events separately. Exceptions/settings are not echoed.

This exercises the native **host** shutdown path, not OS SIGTERM delivery through
`moexctl`. Keep actual CLI signal/exit tests and guardian/client-inventory closeout
receipts separate. Stop success is not an external zero-client inventory proof.
No working-order fault case is admitted; fake tests retain those safety controls.
Neither a PASS here nor a normal healthy CLI exit supplies genuine partial fill,
full-day, auction, interruption or MOEX-coordinated certification evidence.

## Actual CLI signal validation

`moexctl_no_post` compiles the real `apps/moexctl.cpp` main, signal handlers,
command socket and drain loop against the separate hard no-post session archives.
It requires `--qualification-no-post-test` with the ordinary explicit TEST run
inputs, sole instance, original risk ceilings and at least 7,000 remaining IDs.
Kill switch starts on and cannot be released in this variant. Normal `moexctl`
rejects the qualification flag and contains no `qualification-loss` command.

After independently proving flatness and complete native admission, the operator
may send `qualification-loss recover` or `qualification-loss hold` through the
owned command socket. The command closes only this process's connection, polls
native invalidation, and audits the loss. Each process accepts at most one loss.
A real OS SIGTERM is then delivered while reconstruction is pending. The normal
signal handler, cancellation requests, ten-second deadline, second-signal branch,
drain audit and process exit are used without a modeled outcome substitution.
`recover` permits normal reopening; `hold` refuses reopening until process exit.
No router/upstream/network or other client is modified.

Acceptance cases: healthy SIGTERM exits0; SIGTERM after recoverable owned loss
exits0 with current native empty-scope evidence; SIGTERM with held loss exits8
at the ten-second drain; a second real SIGTERM during held recovery interrupts
promptly and exits8 with incomplete reconciliation. Negative exits8 are required
refusals, not normal execution closeout. Verify strict owned PID/start/binary
identity before signals, zero posts, process absence and independent zero clients
and fresh account flatness after each. Do not retry an uncertain control command.
Regression tests deliver real signals through the actual CLI entry point against
an inherited local fake SDK. Native acceptance must use the separately sealed
`moexctl_no_post` binary, preserve all receipts and keep the timer paused.
