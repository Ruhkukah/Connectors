# SPECTRA 9.9 AGGR operator emergency procedure

This procedure applies to the proprietary TEST connector and contains no credentials or private router settings.
It is the operator response for any event that makes the exchange state uncertain.

1. **Unexpected order or position.** Stop all new commands immediately. Keep the host in its fail-closed state,
   preserve the durable journal and raw replies, and obtain fresh POS, TRADE, USERORDERBOOK and PART evidence.
   Do not retransmit an uncertain Add and do not flatten automatically. Reconcile the existing order epoch with the
   broker/MOEX operator before sending one explicit recovery Cancel.
2. **Command or reply ambiguity.** Treat every unknown, system, timeout or contradictory reply as a failure of
   certainty. Record the raw bounded payload, message ID, user ID, causal error and current stream generations.
   Do not infer success from correlation alone.
3. **Router or network loss.** Disable Add and publisher activity, leave the local router running unless MOEX directs
   otherwise, and preserve the last causal transport state. Allow only the bounded recovery path already tested by
   the host. If certainty cannot be restored, stop the process and escalate.
4. **Application failure.** Do not restart blindly with an active order epoch. Preserve the journal/evidence
   directory and hashes, then restart only the reviewed executable with the same configuration after the operator
   has confirmed the recovery plan.
5. **TCS failure or exchange maintenance.** Stop command activity, retain Exchange/NCC messages and the current
   session state, and follow the MOEX notice or coordinated test instruction. TCS restart/reload and access-server
   switching are MOEX-coordinated exercises.
6. **Unable to restore certainty.** Keep all effective readiness and order gates false. Do not cancel, flatten, or
   retry by intuition. Preserve the complete event log and mark the scenario `NOT_READY` or `MOEX_COORDINATED` with
   the reason.
7. **Escalation.** Contact the configured broker operations desk and MOEX technical support through the authorized
   support channel listed in the current test notice. Supply scenario ID, source/binary/runtime/scheme/config
   fingerprints, timestamps, causal error, stream health and order/position identity. Never send credentials, raw
   secret-bearing settings, or private keys in an escalation package.

The operator must record who acknowledged the procedure, the start/end time, the last known order/position state,
and whether the exchange or broker gave a coordinated next step. A scenario cannot be marked PASS while any item
above remains unresolved.
