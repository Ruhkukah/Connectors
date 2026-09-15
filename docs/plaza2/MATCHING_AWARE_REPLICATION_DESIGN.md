# Generic matching-aware replication backlog

Status: forward-compatibility design only. This tranche does not change the
current AGGR candidate's unsuffixed subscriptions and does not add a hard-coded
`FORTS_TRADE_REPL_MATCH1` path.

## Evidence boundary

The preserved 2026-09-15 read-only experiment resolved both
`FORTS_TRADE_REPL` and `FORTS_TRADE_REPL_MATCH1`, and the current status-stream
family also resolved. That result is not evidence that a single matching is a
safe production assumption, and it does not reclassify or rewrite the earlier
r2 evidence. The current correction is limited to external-service recovery.

## Required architecture

The eventual implementation must model replication identity as
`(stream_family, matching_id)`, with service names derived from authoritative
`FORTS_REFDATA_REPL.instr2matching_map` rather than from a fixed suffix.

It must:

- discover every valid active matching ID from committed REFDATA and map the
  target instrument/base contract to its current matching;
- support multiple active matching IDs and assignments changing between
  sessions or trading days;
- keep independent listener ownership, LifeNum, revision, snapshot, ONLINE,
  ClearDeleted and recovery state for every matching-specific stream;
- combine private TRADE/order/trade evidence across every matching relevant to
  the login before declaring reconciliation complete;
- preserve a single fail-closed command authority over the combined private
  surface, including restart checkpoints, uncertain Add state and explicit
  recovered Cancel authorization;
- prove status streams separately because they are not in the documented
  matching-partition list unless the reviewed scheme explicitly says otherwise.

## Scope and review gate

Address this backlog with the later Full ORDLOG tranche, which also depends on
matching-partitioned stream families. Before formal MOEX submission, reassess
which shared AGGR evidence must be rerun after the generic topology and
reconciliation architecture lands. No current certification row is promoted by
this design note.
