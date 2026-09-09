# Plaza II C2 access evidence — 2026-09-08

This tranche continues PR #44 final head `0008f631b7e4df5f12efade5c8b3d796a1bc8407`.
It is an offline evidence and tooling change. No new T1 connection or replication
open was attempted, and the paused 07:00 automation was not changed.

## Retained negative evidence

The source trace remains immutable:

```text
SHA-256: a05afa4e5de25a8818f8cb91057c5cf02ead8f341636f6880e286e9febf83d66
outcome: BLOCKED_REGULAR_REPLICATION_ACCESS_DENIED
time: 2026-09-08 07:01 MSK (approximately)
```

CGate environment and connection setup succeeded. The anonymous composite
listener was accepted locally and then entered `ERROR` after the server rejected
the open request:

```text
REPL:ACCESS_DENIED
40969 / 0xA009
Open request rejected by server
```

No descriptor, snapshot, `ONLINE`, or market-data callback was received. Capture
loss was zero. Publisher calls, transaction commands, and orders were zero. This
does not identify the denied constituent stream and is not an implementation or
certification failure.

## Current external gate

```text
WAITING_FOR_MOEX_FULL_ORDERS_LOG_ACCESS_CONFIRMATION
```

The next live attempt requires written MOEX or access-administrator confirmation
that the intended T1 login has Full_orders_log access, or newly issued credentials
with that property, followed by a fresh explicit authorization. No retry timer is
scheduled.

## Local provisioning audit

The audit was offline and value-redacted. The repository contains only placeholder
or environment-variable references for the T1 endpoint and credentials. Searches
covered tracked `docs/`, `profiles/`, `scripts/`, `apps/`, `tests/`, and `cert/`
for:

```text
Full_orders_log
Полный журнал заявок
ORDLOG
FORTS_ORDLOG_REPL
FORTS_ORDBOOK_REPL
```

There is no authoritative local provisioning record for the current login class or
the requested full-order-log entitlement. The result is:

```text
login_class: UNKNOWN
FULL_ORDERS_LOG_PROVISIONING_NOT_FOUND_LOCALLY
```

`AddOrder` references in historical transactional material were not used to infer
`TRANSACTIONAL`; main IDs may also have transaction authority.

## Prepared offline tools

`tools/plaza2_c2_support_bundle.py` creates a new mode-700 directory containing a
redacted summary, relevant diagnostics, state transitions, and a manifest. It
hashes the source trace but never copies it. Private login or firm metadata can be
passed through a private input file and is never read into repository evidence.

`tools/plaza2_c2_entitlement_probe_plan.py` emits a no-execution plan for one
bounded open attempt for each of `ORDLOG_ONLY`, `ORDBOOK_ONLY`, and
`COMPOSITE_P2ORDBOOK`. The URLs and qualified `CustReplScheme` forms are checked
against the existing 9.9 documentation and tests. The plan has zero retries,
publishers, transaction commands, and orders.

Neither tool opens CGate or contacts MOEX.

## Validation

The retained trace was independently imported into a new derived directory; its
SHA remained `a05afa4e5de25a8818f8cb91057c5cf02ead8f341636f6880e286e9febf83d66`,
with the existing `STATE_ERROR`/zero-callback negative result preserved. The
support bundle then retained the first two diagnostic monotonic timestamps and
excluded raw bytes.

The focused Release, macOS ASan/UBSan, and Ubuntu 22.04 x86_64 ASan/UBSan with
LeakSanitizer suites each passed the C2 preflight, capture, and support-bundle
tests. Style and Unicode guards passed. The fake hot-poll case exceeded 100,000
unchanged polls while emitting at most two error-state frames per listener.
