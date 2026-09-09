# ORDLOG / ORDBOOK schema inventory, Phase A

Base: `22a9dfd0947ccde4645696974e1270099491e2c6`.

All ten reviewed table field sequences match the **local cached 9.9 manual**, including types. This is a field-name/type comparison, not a current vendor binary-layout
qualification. See [baseline](plaza2_certification_baseline_20260907.md) for provenance and blockers.

The generated header provides `StreamCode`, `TableCode`, `FieldCode`, and descriptors. It generates **no typed wire record structs** for these tables. Hex IDs below are repository
identifiers, **not CGate message indices**. Runtime `msg_index` and field offsets come from `cg_lsn_getscheme` / `RuntimeMessagePlan`; no frozen ORDLOG/ORDBOOK native size, offset,
null map or message-index fixtures exist. Reviewed `order` values are inventory order, not wire ordinals.

## `FORTS_ORDLOG_REPL`

### `orders_log`

Repository TableCode value: `0x2D2E1CF1`. Fields: 17. Local manual comparison: MATCH.

```text
replID: i8
replRev: i8
replAct: i8
public_order_id: i8
sess_id: i4
isin_id: i4
public_amount: i8
public_amount_rest: i8
id_deal: i8
xstatus: i8
xstatus2: i8
price: d16.5
moment: t
moment_ns: u8
dir: i1
public_action: i1
deal_price: d16.5
```

### `multileg_orders_log`

Repository TableCode value: `0x823E3B6B`. Fields: 19. Local manual comparison: MATCH.

```text
replID: i8
replRev: i8
replAct: i8
public_order_id: i8
sess_id: i4
isin_id: i4
public_amount: i8
public_amount_rest: i8
id_deal: i8
xstatus: i8
xstatus2: i8
price: d16.5
moment: t
moment_ns: u8
dir: i1
public_action: i1
deal_price: d16.5
rate_price: d16.5
swap_price: d16.5
```

### `heartbeat`

Repository TableCode value: `0xEF612983`. Fields: 4. Local manual comparison: MATCH.

```text
replID: i8
replRev: i8
replAct: i8
server_time: t
```

### `sys_events`

Repository TableCode value: `0x61859552`. Fields: 8. Local manual comparison: MATCH.

```text
replID: i8
replRev: i8
replAct: i8
event_id: i8
sess_id: i4
event_type: i4
message: c64
server_time: t
```

## `FORTS_ORDBOOK_REPL`

### `orders`

Repository TableCode value: `0xADF79085`. Fields: 17. Local manual comparison: MATCH.

```text
replID: i8
replRev: i8
replAct: i8
public_order_id: i8
sess_id: i4
moment: t
moment_ns: u8
xstatus: i8
xstatus2: i8
public_action: i1
isin_id: i4
dir: i1
price: d16.5
public_amount: i8
public_amount_rest: i8
public_init_moment: t
public_init_amount: i8
```

### `multileg_orders`

Repository TableCode value: `0x30E3F205`. Fields: 19. Local manual comparison: MATCH.

```text
replID: i8
replRev: i8
replAct: i8
public_order_id: i8
sess_id: i4
moment: t
moment_ns: u8
xstatus: i8
xstatus2: i8
public_action: i1
isin_id: i4
dir: i1
price: d16.5
public_amount: i8
public_amount_rest: i8
public_init_moment: t
public_init_amount: i8
rate_price: d16.5
swap_price: d16.5
```

### `info`

Repository TableCode value: `0x60AF30C0`. Fields: 8. Local manual comparison: MATCH.

```text
replID: i8
replRev: i8
replAct: i8
infoID: i8
moment: t
publication_state: i1
trades_rev: i8
trades_lifenum: i8
```

### `orders_currentday`

Repository TableCode value: `0xD0C42A63`. Fields: 17. Local manual comparison: MATCH.

```text
replID: i8
replRev: i8
replAct: i8
public_order_id: i8
sess_id: i4
moment: t
moment_ns: u8
xstatus: i8
xstatus2: i8
public_action: i1
isin_id: i4
dir: i1
price: d16.5
public_amount: i8
public_amount_rest: i8
public_init_moment: t
public_init_amount: i8
```

### `multileg_orders_currentday`

Repository TableCode value: `0xD472CAE3`. Fields: 19. Local manual comparison: MATCH.

```text
replID: i8
replRev: i8
replAct: i8
public_order_id: i8
sess_id: i4
moment: t
moment_ns: u8
xstatus: i8
xstatus2: i8
public_action: i1
isin_id: i4
dir: i1
price: d16.5
public_amount: i8
public_amount_rest: i8
public_init_moment: t
public_init_amount: i8
rate_price: d16.5
swap_price: d16.5
```

### `info_currentday`

Repository TableCode value: `0x807D9570`. Fields: 7. Local manual comparison: MATCH.

```text
replID: i8
replRev: i8
replAct: i8
publication_state: i1
trades_rev: i8
trades_lifenum: i8
server_time: t
```

## Consequences for Phase B

- ORDLOG uses `price`, `dir`, `id_deal`: not the plan's illustrative `public_price`, `public_dir`, `deal_id`. These public tables have no `status` or `ext_id`; preserve
`xstatus`/`xstatus2` and do not invent private fields. Multileg records add `rate_price` and `swap_price`.
- Both snapshot variants already exist in metadata. `info` and `info_currentday` use `trades_rev` / `trades_lifenum`, not the obsolete `logRev` / `lifeNum` example.
- Freeze official 9.9 runtime definitions, including packed decimal `d16.5`, timestamp `t`, nullability and actual table indices. A C++ descriptor's storage hint is not proof of a
packed struct ABI. `moment_ns` is unsigned nanoseconds since Unix epoch UTC, not a subsecond remainder.
- The checked-in 9.9 runtime signature stores field counts and signature hashes, not all wire fields/offsets. Its logical-section mapping leaves vendor section names outside the
required consumed set; zero fatal drift there does not certify these new public tables.
- Reuse `tools/plaza2_schema_materialize.py`, `tools/plaza2_codegen.py`, and `tools/plaza2_runtime_scheme_lock.py`; extend generation only for necessary wire access/layout
fixtures. Do not hand-maintain a parallel packed model.
- Required fixture cases per table: size/offset/index, all fields, signed extremes, exact decimals, full timestamps, null bitmap, unknown status/action bits. Replication receive tables do not require an encoder.
- Generic runtime descriptors can recognize these tables, but there is no dedicated live ORDLOG/ORDBOOK session, raw output, recovery coordinator, projector or benchmark. AGGR20
configuration explicitly rejects these streams.
