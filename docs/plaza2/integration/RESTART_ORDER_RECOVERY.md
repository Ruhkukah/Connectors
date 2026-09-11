# Persistent checkpoint restart reconciliation (offline)

A nonterminal v2 checkpoint starts an explicit `RestartOrderRecoveryRequired` owner. It never restores Add
capability, including after terminal reconciliation. `plan`, `authorize`, `begin_order`, `submit_order` and normal
Cancel cannot bypass this mode. Only fresh positive terminal proof or a newly approved exact recovered DelOrder
can resolve the old question. A new ordinary owner is possible only after a safe terminal checkpoint is committed.

## Durable identity

`moex.connector_host.persistent_session.v2` retains phase, epoch/base/run/profile identity, profile fingerprint,
full account fingerprint, exact isin/session/side/price/quantity and original ext/Add/normal-Cancel/recovery IDs.
It also records original Add authority and payload hashes, session-price binding hash and generation, positively
known order ID/remaining quantity, last observed lifecycle state and next recovered-Cancel reply ID.
The account hash is a comparison aid, not protection against brute-force recovery of short participant codes.
Raw credentials are not added. Protected configuration must match the historical identity; session retargeting
and price/side/account substitution are refused. A legacy nonterminal v1 file is never converted by guessing.

Checkpoint writes use exclusive no-follow 0600 temporary files, file fsync, rename and directory fsync.
A held advisory file lock prevents concurrent checkpoint owners. A durable `.required` marker makes a missing
checkpoint fail closed; deleting the checkpoint is not a recovery operation. Deliberately deleting all safety
files is outside the API and never a documented way to regain Add. V2 accepts only its exact canonical encoding,
rejecting duplicate, unknown, malformed or incomplete fields. Current files are bounded protected regular files.

## Fresh truth and protected approval

Startup performs the existing full qualified bootstrap unchanged. The checkpoint supplies identity only; it
never establishes Working or terminal state. The same PR59 matcher requires fresh POS anchor, TRADE, separate
UOB census, PART, relevant reference/status streams, AGGR and active transport handles. Missing, conflicting,
multiple or ambiguous matches remain blocked. Fresh Filled/Cancelled proof closes without a command.

The restart transport stores identity in a recovery-only context; it does not install/bind an Add plan. A new
journal under a random process identity preserves the prior process journal. Exact approval additionally binds
the schema and hash of the checkpoint loaded by this process, historical epoch, random process identity, fresh
transport generation, POS anchor, exact reconciliation, original Add authority and exact DelOrder payload.
Approval is a new exclusive 0600 artifact and exact SHA. Previous-process artifacts are never imported.

The descending reply-ID reservation is persisted before even creating an approval artifact. The checkpoint
therefore burns prepared and abandoned IDs as well as attempted ones. Ordinary IDs bound the available range;
exhaustion blocks. Each new process must reconcile and obtain a new operator approval even when a historical
receipt said `post_invoked=false`. Consumed marker and per-attempt submission receipts remain durable. There is
no automatic Add, Cancel, mass cancel, flatten or uncertain-command retry.

## Offline coverage and limits

Tests include real child-process exits after Add before reply and after Working/normal Cancel, then restart.
Recovered-Cancel windows: A prepared artifact, B durable consumed marker before publisher (using the same
consumption primitive), C fake publisher allocation before post, D post invocation before reply, E reply without
private terminal evidence. Fresh bootstrap posts zero until the explicit approved call. Tests cover old artifact
rejection, persistent distinct reply IDs, profile/account/session/price mismatch, absent/conflicting evidence,
fresh full fill without Cancel, v1 rejection, missing checkpoint and ID exhaustion. Existing strong matcher tests
retain multiple/lookalike and partial-fill quantity cases. The public one-lot host cannot partially fill an
indivisible contract; the shared matcher still accounts for exact executed and remaining quantities.

This is offline implementation, not live certification. PR55 transport recovery rules are unchanged. No VPS or
T1 operation, exchange order, merge or performance refactor is part of this work.
