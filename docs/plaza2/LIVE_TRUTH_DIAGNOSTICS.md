# PLAZA participant identity and live diagnostics

Base: `82707d4e00b1bb6bf703be800ff31988990116df`. Historical T1 evidence belongs to
`96199b6ca5b1141c01670830be067bfd69c5ed71`; implementation changes do not amend it.
This change is offline. No deployment or live order is authorized by these diagnostics.

## PART identity and sending policy

`part.client_code` identifies a client or brokerage firm. PART now uses
`LimitParticipantKind`, independently of POS `PositionScope`. Four uppercase ASCII
alphanumeric characters identify a brokerage candidate; seven identify a client
candidate. All other shapes remain Unknown. This conservative classification is
not proof that a configured account is entitled to trade.

Replication IDs retain distinct rows for duplicate-account detection and permit a
slot to change account identity. The code index is rebuilt when the existing
committed limit projection is published. Target lookup reads the authoritative
committed map through that index; polling and pre-send no longer scan limit vectors.
A lookup with multiple rows returns a count and no selectable row. Transaction
staging remains the existing implementation; the wider delta refactor is separate.

The host reports row presence, exact client identity and exchange financial checking
separately. Sending still requires one exact full-client row with `limits_set=1`.
A matching brokerage row, Unknown row, duplicate, unchecked limit, or `client_code=000`
does not grant an alternative send path. POS handling of 000 is unchanged.
`limits_set=0` means financial limits are not checked; it does not establish account
rejection or insufficient funds.

Explicit qualification snapshots include broker/client match counts, unknown-row
count and the 000-profile flag. `participant-limits.json` uses participant kind,
code length, equality booleans and money fields. It does not publish account codes
or claim that hashes of short account codes provide secrecy. Treat money diagnostics
as private evidence and redact before public publication.

The C ABI layout is unchanged. `MoexPlaza2LimitItem.scope` now uses the independent
`MoexPlaza2LimitParticipantKind` constants: Client=0, BrokerageFirm=2, Unknown=3.
POS scope constants and values are unchanged.

## Target reference-row forensics

The qualification observer opts into the configured isin/session only. Supply
`MOEX_AGGR_FORENSIC_SYMBOL` with the independently selected contract symbol for a
symbol-identity comparison. No order-price formula has changed.

`target-forensics.jsonl` records committed target revisions, UTC/monotonic commit
times, LifeNum, negotiated table index/name, message size, raw payload and null map.
For identity, limit, settlement, deposit and price-step fields it retains negotiated
name/type/offset/size/ordinal, null state, raw bytes, the generic decoded value, and
a second direct `cg_getstr` conversion using the negotiated name/offset route.
The latter does not select its input through a generated FieldCode lookup.

Target numeric identity, symbol identity and presence in AGGR are separate checks.
Absence of a supplied symbol is not proof of symbol identity. Decoder disagreement
blocks orders. The qualification price gate also requires a committed, independently
matched target identity, expected symbol and corresponding AGGR instrument; a missing
symbol or missing probe is not an authorization. Rows are bounded to 64 KiB, 4,096 null entries and 128 descriptors;
the observer holds at most 16 undrained target rows. Rollback/close/LifeNum does not
publish pending rows. Relevant ClearDeleted boundaries remove captured revisions
strictly older than the boundary, including already-drained identity proof. Surviving
revisions retain their eligibility. Effects within a transaction are staged until
commit; MAX retires prior publication evidence and permits fresh low revisions,
including a revision number used before the reset. Other tables do not affect the
target. LifeNum/Close/Open retire undrained evidence as well as verified proof.
Formatting and disk I/O occur outside callbacks.

No new live target payload has been collected by this implementation. Consequently,
the historical 179/179 values remain unexplained. If independent raw decoding
confirms those absolute bounds against a market around 2100, retain
`RAW_T1_PRICE_LIMIT_VALUES_INCONSISTENT_WITH_CURRENT_MARKET` and ask MOEX about the
specific T1 instrument/session/descriptor. Do not derive settlement +/- 179.
Moving authoritative session terms out of the qualification collector is deferred
until this forensic comparison is resolved.

## Raw versus effective health and errors

Raw flags retain the last snapshot/handle information. Effective private, AGGR,
publisher and reply readiness requires current ACTIVE transport objects, valid
component state and a live host. Failure clears effective readiness immediately
without pretending that historical snapshot rows disappeared. Order receipts and
the final authorized publisher gate use effective transport health.

Start/poll return the original `Plaza2Error`, retaining its connector classification,
raw runtime code and message. Snapshots preserve operation, monotonic failure time,
first callback diagnostic and connection/publisher/reply/AGGR/private object states
at the failure, separately from current object states. Internally generated errors
have no raw CGate code (`null` in JSON), rather than a fabricated runtime result.

Router recovery is intentionally not implemented in this PR. It belongs in the
next independently reviewable PR and must retain no-resend order semantics.
