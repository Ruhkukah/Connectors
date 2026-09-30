# Public trades through ConnectorHost and DTC

The opt-in `--public-deals` flag on `moex_connector_host_dtc_runner plaza2 qualify`
adds `FORTS_DEALS_REPL` to the existing read-only REFDATA, SESSIONSTATE,
INSTRUMENTSTATE and AGGR20 listeners. All five use the server-published scheme
and `mode=snapshot+online`. Dedicated DTC authentication remains configured by
`--require-dtc-auth`. No private TRADE, position, account, publisher or reply
surface is part of this profile. The existing four-listener profile remains the
default for clients requiring only depth.

## Trade semantics

Only ordinary `deal` rows for the selected current `sess_id` and `isin_id`, with
`replAct=0` and `nosystem=0`, can become live trades. Multileg and technical trades
are outside this first scope. Rows seen during snapshot bootstrap are not replayed
as new trades. Live rows become visible only at `TN_COMMIT`. Close, callback errors,
invalid transactions, connection recovery and LifeNum changes retire the previous
generation. ClearDeleted is replication maintenance, never a synthetic trade.

The retained trade queue is bounded. The DTC client starts at the current tail on
subscription. A client that falls behind the retained queue or crosses an epoch
must receive a visible failure and establish a fresh subscription; silent loss is
not success. A quiet market can legitimately produce no new trades, so an empty
observation is insufficient evidence of end-to-end live trade delivery.

Trade time comes from the exchange's `moment_ns` field, which MOEX defines as UTC
Unix nanoseconds. The runtime-decoded `moment` calendar counter is retained
separately; it is not the original P2TIME bytes and does not establish its
timezone interpretation. Prices are decoded as exact `d16.5` fixed point before
conversion to the DTC double representation. Trade size is contracts.

Direction uses MOEX's documented `ActiveSide=0x20000000000` and
`PassiveSide=0x40000000000` flags in `xstatus_buy`/`xstatus_sell`: an unambiguous
active buyer and passive seller is DTC AT_ASK (2), and the converse is AT_BID (1).
Absent or contradictory flags yield UNKNOWN (0). Neither order-ID magnitude nor
book changes establish aggressor direction.

Source: [MOEX Plaza II 9.9 data and flag definitions](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/prod/docs/p2gate_ru.html).

## Wire contract

DTC 101 subscribes/unsubscribes to public trades for the validated 507 target.
DTC 107 carries each subsequent trade. The existing source/definition gates
remain required. Capability `MarketDataSupported` reflects whether public deals
is configured; depth support alone never implies trade support.

For that opt-in capability, 507 includes the coordinated `SourceSessionID`
extension (protobuf tag 35) from its validated current REFDATA definition. Kairos
checks each 107 against that session and `SecurityIdentifier`; it does not equate
the independent DEALS and AGGR listener epoch counters. Depth-only 507 remains
unchanged. Tag 35 is a Connector/Kairos extension, not an official DTC field.

| Protobuf tag | Meaning | Wire value |
| --- | --- | --- |
| 1 | SymbolID | subscription ID |
| 2 | AtBidOrAsk | 0 unknown, 1 at bid, 2 at ask |
| 3 | Price | double |
| 4 | Volume | double, contracts |
| 5 | DateTime | double, UTC Unix seconds |
| 6 | StreamEpoch | public deals generation |
| 7 | DeliverySequence | per-session delivery ordering |
| 8 | TradeID | exchange `id_deal`, string |
| 9 | CaptureSequence | committed native trade sequence |
| 10 | ExchangeTimeUnixNs | exact `moment_ns` |
| 11–12 | EngineIngress/EmitUnixNs | unset unless separately measured |
| 13 | SourceReplID | exact replication row identity |
| 14 | SourceReplRev | replication revision, not a contiguous tick sequence |
| 15 | SourceLifeNum | exchange replication LifeNum |
| 16 | SessionID | exchange session |
| 17 | IsinID | exchange instrument |
| 18 | SourceReceiveTimeUnixNs | locally measured callback receipt time |
| 19 | SourceMomentRaw | runtime-decoded `moment` calendar counter, not raw P2TIME bytes |
| 20 | SourceMomentNs | original UTC nanoseconds |
| 21–22 | SourceXstatusBuy/Sell | original direction/status evidence |

Tags 1–5 follow the [DTC trade message](https://www.sierrachart.com/index.php?page=doc/DTCProtocol.php).
Tags 6–22 are coordinated Connector/Kairos extensions, not a universal DTC
interoperability or exchange-certification claim. Historical backfill is absent:
the chart accumulates trades from the current subscription onward.

## Acceptance evidence

Required checks cover native decoding and schema mismatch rejection, transaction
commit/rollback, bootstrap exclusion, target/session filtering, duplicate replay,
generation retirement, bounded buffers, DTC 101/107 wire decoding, independent
Kairos consumption, neutral unknown-side rendering, retained depth history and
the absence of private/order/publisher calls. Live readiness additionally requires
a fresh T1 gate and a bounded run with actual post-subscription deals matched
between Connector and Kairos by identity, price, quantity and time.

Execution results and outstanding certification requirements are maintained in
[the readiness register](review/CONNECTOR_CERTIFICATION_READINESS_20260928.md).
