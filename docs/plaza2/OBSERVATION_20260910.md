# September 10 observation harness

Reviewed functional base: f966a7348a3252db98c8e9e363e7b6005e009e99.
The executable reports its actual build source SHA through --version and environment.json.
Historical source 96199b6 and its evidence remain separate.

Only September 10 2026, 06:58 inclusive to 16:10 exclusive MSK, with
MOEX_AGGR_T1_AUTH=20260910_AGGREGATED_OBSERVATION, is accepted.
Any MOEX_AGGR_T1_ORDER_AUTH value (including empty) is rejected. Configuration
must be Qualify, LiveTestPreSend, send arm false. This runner contains no order
request ingestion or Add/Cancel calls. Publisher/reply handles remain part of
normal observation topology. An active epoch, attempted submission, publisher
post or non-flat observed account census fails the observation.

The protected adapter requires --symbol, --isin and --session, verifies package
hashes and rejects old output or nonempty journals. It cannot enable order/idle
mode. Prepare the output parent and a new empty journal tonight; the executable
creates the previously nonexistent scenario directory at launch. This preserves
its existing exclusive-create safeguard. Resolve the current session tomorrow;
11702 is historical, not a September 10 default. The adapter sets the exact
forensic symbol. Journal/run/profile and unused ID namespaces are September 10.

PART diagnostics add the existing repl_id without raw account codes or changing
authorization rules. Forensic semantics and bounded recovery are unchanged.

The one-shot 06:50 MSK wake-up performs preflight, not an automatic connection.
No connection today, none before 06:58 tomorrow, and no launch with unresolved
current target identity or other prerequisites. Retain at least ten stable
minutes after full bootstrap when evidence is available. Stop after the truth
report. No router fault, orders, ORDLOG or merges are authorized.

Validation: the native self-test covers date/time/token/order-variable boundaries,
non-send configuration and redacted repl_id rendering. A structural test proves
that order.request cannot reach any order API through the entry point. Run full
Release plus PLAZA/sanitizer suites and repository guards. Linux sanitizer runs
must include LeakSanitizer; sanitizer timing is not performance evidence.
