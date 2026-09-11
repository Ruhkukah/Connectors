# Explicit recovered-order cancellation (offline implementation)

The persistent native ConnectorHost exposes reconcile_recovered_order(), prepare_recovered_cancel(path), and
cancel_recovered_order(path, exact_sha256). No recovery transition, poll, environment flag or startup setting
invokes cancellation. The only publisher command in this path is an exactly encoded DelOrder (461), with the
existing arming, ACTIVE-handle and rate gates. Add and DelUserOrders remain unavailable across a quarantined
generation.

## Scope and identity

This capability handles an active in-process persistent epoch across transport rebootstrap. It requires a
strictly newer transport generation, full fresh private ONLINE snapshots, fresh POS-derived TRADE anchor,
current target reference provenance, exact PART client identity, consistent USERORDERBOOK census and current
AGGR readiness. Failed/corrupt transport and incomplete generations cannot authorize it.

TRADE regular orders remain the lifecycle source. USERORDERBOOK is retained as a separate census; it is not
merged into TRADE or required to manufacture a matching row. An explicit conflicting candidate on that census
blocks cancellation. Matching binds the account, numeric instrument/session, ext_id, side, exact scaled price,
original intended quantity and known exchange identifier where available. A price/side/quantity lookalike is
insufficient. The narrow ordinary-order path rejects multileg rows, split public/private identifiers or
quantities, conflicts and ambiguous actions. Own deals require exact account/session/instrument and exchange
identities; ext_id alone cannot override them. The original quantity comes from the persistent intent:
replicated amount fields describe operations, not a replacement order intent.

Only EXACTLY_ONE_WORKING_MATCH can create a cancel artifact. Full fills and positive cancellation evidence
yield TERMINAL_ALREADY and normal journal completion without a command. NO_MATCH alone is not terminal proof.
Conflicts, multiple matches, stale generation, missing fields and insufficient fill evidence remain blocked.
Ordinary-order identity scope follows the official PLAZA guide:
https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/docs/p2gate_en.html . This implementation has no
iceberg or cross-session replacement authority.

## Authorization and evidence

Preparation preserves the prior epoch journal beside the new artifact before any cancel result updates its
current fields. The canonical artifact contains a new nonce, original Add authorization SHA, recovery
generation, epoch, exact account/order/isin/session/side/price/remaining quantity, POS anchor, separate UOB
facts, reconciliation hash and exact cancel payload hash/user_id. It contains private account information and
must remain private; it is not a public evidence export.

Artifacts are exclusively created with mode 0600 and file/directory durability barriers. Execution requires
the exact hash and the same currently prepared path, polls once more, and compares current reconciliation
facts. A durable exclusive consumed marker is written before the publisher call. There is at most one consumed
cancel attempt per recovery generation, including definite non-send failures. No automatic retry occurs. A
subsequent real loss requires another fresh bootstrap, reconciliation and newly approved artifact. Old
artifacts cannot authorize in a new generation or a new process.

The previous journal, consumed marker and per-attempt submission receipt remain on disk. Receipts include
publisher allocation/post counts, submission certainty and raw allocation/post runtime codes; the existing
epoch journal records replies and private terminal evidence. A mandatory receipt failure leaves the epoch
quarantined. The method's returned cancel_submission describes that call; retained lifecycle history remains
in the journal.

Process-restart cancellation is outside this narrow in-process capability: a restored nonterminal checkpoint
remains blocked under the existing restart reconciliation rules. The one-shot lifecycle runner is not upgraded
into an interactive recovery operator. Use the persistent ConnectorHost surface for the proposed future order
harness. There is no CLI or dated harness added here.

## Validation and status

Deterministic fake-runtime cases cover Add post before reply, Working then loss, normal Cancel post before
reply, recovered Cancel post before another loss, a stale approval across another generation, and positive
terminal proof without cancellation. They assert publisher counts, no automatic Add/Cancel, exact new hash
approval, one-shot consumption and ordinary journal completion. Model tests additionally cover full/partial
fills, no match, duplicate/ambiguous/conflicting identities, lookalikes, generation changes and protected-file
mode/symlink/reuse rejection.

All original process/open INTERNAL, proven-identity INVALIDARGUMENT, timeout, deadline,
callback/schema/decode/configuration and zero-order recovery tests remain in the complete suites. ASan exposed
a temporary POS-anchor reference in the new proof builder; it was replaced with a value copy before
publication and the suites were rerun. This is offline implementation evidence, not live Working-order
recovery qualification.

No VPS mutation, T1 connection, exchange order, merge or performance optimization. First live order and
Working order across transport loss remain NOT YET TESTED.

Recovered attempts reserve distinct descending 32-bit reply identifiers across
all epochs owned by the transport. Ordinary epoch identifiers cannot overlap
that reserved range. The new identifier is part of the protected authorization;
the persistent controller expects that identifier for the new reply while the
prior journal retains the old attempt. A late old reply cannot be accepted as
the new Cancel reply. Identifier exhaustion fails closed.
