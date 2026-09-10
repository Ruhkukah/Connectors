# September 11 observation candidate (not deployed)

Stacked on corrected PR #53, `07460fc06562ebad395530aaceb628168c5557a2`.
The executable `plaza2_aggr_qualification_20260911` shares the observation implementation but has
its own fixed September 11, 2026 authorization token and run/journal identity. The existing
September 10 executable retains its fixed date; neither accepts a runtime date override.

The launcher requires a manifest with the exact source SHA, binary SHA-256 and
`authorization_date: 2026-09-11`, plus hashed package/runtime/scheme/configuration files.
The local preparation receipt binds the candidate configuration and planned fresh evidence/journal
roots. Those directories are created only by a future authorized run. No old evidence is reused.
Fresh September 11 instrument/session identity, T1 availability, exact PART identity, zero exposure
and zero active orders must be established again; September 10 observations are historical only.

## Review and live gate

Stop before deployment. No VPS mutation, T1 connection, router signal, order, or timer is part of
this preparation. Passing offline tests do not authorize deployment. Preserve the earlier live
router FAIL and old-classifier negative-control receipt unchanged.

After separate review and authorization, independently check current MOEX T1 availability notices
and the dedicated router, verify all source/binary/runtime/scheme/configuration hashes, and bind the
fresh target instrument/session. Use new evidence and journal roots for this exact candidate.
Observe full fresh startup with zero orders/exposure before disrupting the dedicated router once.
Leave it down through at least one failed bounded ConnectionOpen reconnect attempt; restart the
same router, require same-process fresh bootstrap and stable effective readiness, then graceful
SIGTERM. Preserve failure and stop if any requirement fails; do not patch/repeat live.

No order.request ingestion, Add, automatic Cancel, cleanup or flatten is permitted.
POST_RECOVERY_OPERATOR_CANCEL_NOT_IMPLEMENTED is unchanged. The confirmed price formula,
account profile, PART matching, POS-to-TRADE dependency, AGGR and shutdown behavior are unchanged.

## Offline checks

Both fixed-date executable self-tests accept only their own date/token/window, reject adjacent
dates and any order authorization variable. Shared structural tests exclude order API calls from
main and retain SIGTERM/SIGINT handling tests. Release/sanitizer counts and exact source/binary
identities are recorded in the external preparation receipt after compilation.
