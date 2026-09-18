# AGGR late-join architecture and remaining dependencies

Source baseline: `2c5464a96f2014b3d53f3f32015a4d4a7dbc47d5` (sealed experiment).
Descriptions below reflect the implementation inspected in the September 18 working
tree on that baseline, including uncommitted changes. They describe source behavior,
not successful live execution, completed testing or exchange acceptance.

## Authority and display

`protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_aggr20_md.hpp` defines
`Plaza2Aggr20AuthoritySnapshot` and `Plaza2Aggr20ListenerBridge`. The current bridge
retains separate committed snapshot and online witnesses, exposes aggr_online and
clears witnesses on invalidation. Its source authority enum remains separate from
the new SessionReadyWitnessKind classification. Snapshot evidence is not promoted
to the online barrier. Open/LifeNum/Close and ClearDeleted recovery paths fence
state through the bridge's invalidation logic.

The host/DTC surfaces separately expose transport_active, snapshot_complete,
aggr_online, book_snapshot_current, session_ready_witness,
session_ready_witness_kind, session_tradable, instrument_tradable,
market_data_display_allowed and order_entry_allowed.

- OnlineSynchronousEvent: this application received the matching event online and
  committed its transaction. Source witness, not sufficient account/order authority.
- PersistedOnlineWitness: durable evidence of that actual online observation,
  revalidated against fresh session identity. Do not manufacture it from a snapshot;
  restart reuse and invalidation require an explicit contract. No implemented
  persistence claim here.
- LateJoinCorroboratedSnapshot: fresh completed AGGR snapshot + ONLINE + committed
  type-1 row for exactly the current sess_id, independently corroborated.
  Provisional display only; no MOEX confirmation received.

`connectors/connector_host/include/moex/connector_host/late_join_display.hpp`
implements `late_join_display_corroborated`: active transport; completed online
REFDATA/SESSIONSTATE/INSTRUMENTSTATE snapshots; exact current-session snapshot
witness; three REFDATA row provenances under the current REFDATA LifeNum; and one
unambiguous session whose main, morning or evening window contains the current time.
It requires known session/instrument public states, a current-session futures member,
and nonzero trade_mode_id. Recovery errors prevent corroboration.

INSTRUMENTSTATE has no wire sess_id. The host uses the projected instrument's
REFDATA session membership plus current status and stream readiness; this is a
local corroboration policy, not a documented cross-stream synchronization barrier.
MOEX confirmation of its adequacy remains outstanding. Known halt/auction/close-only
states can corroborate identity without granting trading permission.

Both online and snapshot witnesses require fresh, unambiguous current session
identity. An old online witness cannot bypass session-window expiry. Instrument
status carries a separate REFDATA-binding flag: REFDATA invalidation or membership
change revokes that association even if a previously observed status value remains
available for diagnostics. Status received before membership cannot certify it.
The session host refreshes the two public status listeners with fresh
`mode=snapshot+online` snapshots after a committed membership generation changes;
it does not depend on an otherwise idle status stream spontaneously publishing a
new row. Display remains withheld until independent current evidence is restored.

`connectors/connector_host/include/moex/connector_host/dtc_market_data.hpp` exposes
`DtcMarketDataSnapshot`, `DtcMarketDataSource`, `DtcFrameDecoder` and read-only
capabilities. `src/dtc_market_data.cpp` now gates `valid` on
`market_data_display_allowed` and preserves witness classification. The host's
`apply_market_data_display_authority` permits corroborated target snapshots for
display while retaining separate source_consistent/target_authoritative semantics.
Both host display policy and DTC adapter force order entry false. MOEX confirmation, if
obtained, can confirm a source criterion; it cannot by itself arm this DTC provider.

`DtcReadOnlyServer` implements a bounded loopback TCP endpoint against this source
boundary: encoding negotiation, depth-only logon, security definitions, snapshots,
heartbeats, disconnect fencing and bounded backpressure. It exposes no order or
publisher surface. Source authority is transmitted separately using the versioned
`moex.source_authority.v1` USER_MESSAGE envelope; diagnostic logs are not authority.
Server batch sequence is independent of source replRev. replID and replRev remain
SourceRowID and SourceSequence, and finite signed/zero prices remain valid.
Do not infer trades or individual orders from AGGR depth.

An actual C++ socket fixture consumed by the Kairos PR #108 adapter passed on
September 18: ALRS-12.26/FORTS, bid -1.0 x 5, ask 0 x 7, epoch 73, version 9,
watermark 55, hash 1234, source row revisions 54/55, Cyrillic UTF-8 description,
and explicitly configured currency value per increment 1.0 RUB. The consumer
reported LateJoinProvisional with exchange confirmation and order permission false.
This is cross-repository fake-source integration, not live CGate or rendered-UI proof.

## Full-day observer

Reuse the read-only structure of `apps/plaza2_aggr20_authority_probe.cpp` and
`protocols/plaza2_cgate/src/plaza2_aggr20_authority_probe.cpp`.
`ProbeAggrHandler` already records sys_events with transaction membership and
before/after-ONLINE provenance; `has_ready_after_online` explains the old result.
The existing two bounded attempts are not a full-day observation and do not establish
fresh status-stream corroboration. The current `apps/plaza2_day_observer.cpp` and
`apps/plaza2_day_observer_journal.hpp` add a separate four-stream observer for AGGR,
REFDATA, SESSIONSTATE and INSTRUMENTSTATE. It requires explicit read-only TEST
arming, pins runtime/scheme/T1 configuration and localhost router port 4101, and
offers an offline fixture. The default duration is 86400 seconds.

Its journal preserves row fields/raw bytes, transaction boundaries, ONLINE,
LifeNum/ClearDeleted and generations. Availability failures create recorded gaps
and fresh-snapshot generations; callback/decoding/evidence failures remain terminal.
Correlate cross-stream events by event_id; never impose a cross-stream replRev order.
The presence of this implementation does not prove a full-day capture has run.

The recorded value `1789662416`, interpreted as Unix seconds, is
`2026-09-17T16:26:56Z` / `2026-09-17 19:26:56 MSK`: the previous evening, not
September 18 morning. The decoder uses timegm on vendor calendar fields, so raw
vendor timezone semantics still warrant verification; neither interpretation makes
this a newly published September 18 morning event. Start before the preceding day's
relevant lifecycle and continue through the next day's initialization and full T1
schedule [S7 in SOURCES.md]. Starting shortly before 07:00 can miss the witness.
Keep lossless
capture outside Git, rotate without dropping records, expose queue age/overflow
and gaps, and write compact UTC/MSK timeline derivatives. Do not overwrite this
September 18 root. Full-day operation and its measurements are not executed here.

## Game and public ORDLOG parallel tracks

`protocols/plaza2_cgate/src/plaza2_runtime.cpp` recognizes `links_public.game.ini`
and `game.ini`; recognition is not provisioned access. The September 18 live
existence check found no `runtime/game.ini`, `links_public.game.ini` or
`plaza2_game.env` in the authorized TEST paths. This establishes no provisioned
Game configuration in the checked paths, not that the exchange Game service is
unavailable. The spec-lock manifest lists Game files as uncommitted/pending local
locks. Provision a separate TEST-only
router/configuration/credential overlay and external evidence root, verify actual
Game endpoints, entitlement, release/scheme hashes and fresh identities, then run
a bounded read-only connectivity/REFDATA/status/AGGR capture. No Game credentials,
availability, equivalence to T1 or completed qualification are established here.

`protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp` explicitly rejects public ORDLOG,
ORDBOOK and DEALS in the AGGR runner. Public full-order reconstruction is therefore
a separate consumer, not a flag enabling L3 on AGGR. Follow the existing
`docs/plaza2/MATCHING_AWARE_REPLICATION_DESIGN.md` backlog: discover matching IDs
from committed REFDATA instr2matching_map; own listeners, LifeNum, revisions,
snapshot/ONLINE and recovery per (stream_family, matching_id). Do not hard-code
MATCH1 or assume status streams have the same partition topology.

Use `docs/plaza2/P2ORDBOOK_ORDLOG_RECOVERY_9_9.md` and
`PUBLIC_L3_MUTATION_CONTRACT_9_9.md` as existing design inputs, not proof of a live
implementation. Private orders_log is not market-wide ORDLOG. Qualify reconstruction,
lossless overload behavior and target-Linux performance with actual source traffic
before claiming full ORDLOG readiness; synthetic/replay throughput is insufficient.

## Encoding boundary

The exact observed Windows-1251 name bytes and UTF-8 value are in evidence.json.
The current `protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_text.hpp`
implements explicit Windows-1251 fixed-string decoding, retains ASCII, replaces
undefined byte 0x98 with U+FFFD, validates UTF-8 and escapes malformed JSON string
bytes. Forensic raw bytes remain distinct from decoded text. Runtime and serializer
integration has exact-name JSON roundtrip, all-byte CP1251 oracle, actual runtime
listener fixtures, DTC protobuf strings and system-message tests. The cross-repository
socket fixture additionally verifies the Cyrillic security definition at the Rust
consumer. This does not retroactively repair the sealed original JSON: its raw
bytes and hashes remain unchanged.
