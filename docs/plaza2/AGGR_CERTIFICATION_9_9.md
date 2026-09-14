# SPECTRA 9.9 Aggregated-mode certification readiness

Status as of 2026-09-14: **AGGR_NOT_READY_FOR_MOEX_CERTIFICATION**.

This report starts at merged main `78f1dded089453d8e3d52a3f1fc26536baf1b197`. It covers the SPECTRA 9.9 Aggregated Order Book product only.
Full public ORDLOG/ORDBOOK/p2ordbook mutation remains `DEFERRED_FULL_ORDLOG_PHASE`; that scope is not silently promoted by this report.

The test trading session is finished for today. No order, router restart, connector restart, or VPS mutation is performed in this tranche.
The dedicated machine-readable identity and matrix are [the 9.9 AGGR manifest](../../cert/aggr_plaza2_certification_manifest_9_9.json) and [the AGGR matrix](../../cert/AGGR_CERT_MATRIX_9_9.md).
The next-session procedure is the [current AGGR T1 runbook](AGGR_T1_QUALIFICATION_9_9.md).

The matrix labels `C01-C08`, `R01-R08`, and `S01-S07` are internal stable IDs mapped one-to-one to
the numbered Plaza II controls in Appendix 1 of the current [MOEX VPTS certification procedure](https://www.moex.com/ru/documents/4531)
(approved 30 January 2023, order МБ-П-2023-207); they are repository traceability labels, not MOEX-named identifiers.
The current document is pinned by PDF SHA-256 `91c24dd5d03947e03f4d2b9fa3a78ab1b4b9d5c8fc43dc91e9e7205135caf2f1`.
The current [VPTS connection requirements](https://www.moex.com/ru/documents/11844) (edition effective 17 August 2020)
are pinned by PDF SHA-256 `a775f5c5aca3cefba58498549d8ff076055091faea757a6a0fbf1ba848f4a4a2`.
The current Appendix 1 groups remain equivalent to the existing mapping (Connection 1-8 -> C01-C08,
Replication 1-8 -> R01-R08, Sending 1-7 -> S01-S07), so no implementation remap is required.

## Runtime and official-source lock

The current official T1 FTP listing was refreshed on 2026-09-14. The active Linux distribution remains
`cgate_linux_amd64-9.9.1853.zip` and the locked Linux package remains `cgate_9.9.0-2008_amd64.deb`;
the Windows listing `setup_SpectraCGate_x64_v9.9.1887.msi` is not the Linux runtime used by the VPS.
No FTP fetch is required to continue this audit because the VPS package and scheme fingerprints already match
the current lock. If MOEX publishes a new Linux archive, the archive must be fetched and re-locked before the next candidate.

| Identity | Value |
|---|---|
| CGate runtime | `6.102.0.6118` |
| Runtime library SHA-256 | `f63e726a8482b793c3af755a8dc2b9ebb5cd727d88fb58ebb3fe9704a155ce6f` |
| SPECTRA marker | `SPECTRA9.9.0` |
| DDS version | `990.1.6.42744` |
| `forts_scheme.ini` SHA-256 | `7b93117ee435fd0cb2849b677fc32a9d581364b6ee9afeac9c6c002875400746` |
| distribution SHA-256 | `49da634749203a919e69132594198348652acb5e731416c8a39bb34969364ce2` |
| runtime compatibility | `CompatibleWithWarnings`, fatal drift `0` |
| router | `P2MQRouter-229.123.0.7233`, binary SHA-256 `6ea6ad50d6e3300fee99e43900eb22a21c207166f2455d2fc850f7e30bc91423` |
| observed Linux executable | SHA-256 `f62fd2a5ed6d68be99433e1e837bed9b246d7ee4dd403edf7c8c2723d264bead` |
| private runtime config fingerprints | `client_t1.ini` `2d3b265ad71fa28d7b5cb448f5e5dd3ec157b4791cf129b205b10be987ed50b3`; order profile `69cac88494e23bb2dd122c9ca888bb9b364bf7d5b790a6dd89064c7de5bc9ef0` |

The official references used for the refresh are the [MOEX T1 FTP listing](https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test),
[T1 schedule](https://www.moex.com/s438), and [SPECTRA 9.9 change list](https://www.moex.com/media/spisok-izmenenij-v-versii-9-9.pdf).
Credentials and raw router/config files remain private and are represented only by fingerprints.

## AGGR topology and safety boundary

The production host is one single-owner `Plaza2TestSessionHost` thread. It owns one TEST connection to the dedicated local T1 router.
It owns five private streams (`POS`, `PART`, `TRADE`, `USERORDERBOOK`, `REFDATA`) and two status streams (`SESSIONSTATE`, `INSTRUMENTSTATE`).
It also owns the `FORTS_AGGR20_REPL` listener, one publisher, and one `p2mqreply` listener.
`TRADE` opens only after a fresh POS.info anchor is committed. Every stream must be ACTIVE and snapshot/ONLINE before effective readiness can become true.

The candidate declares only ordinary `AddOrder` 474 and `DelOrder` 461. Their business replies are 179 and 177 respectively;
system replies 99/100 may also occur and are never ordinary success by correlation alone. `DelUserOrders` 466 has business reply 186
but is known and undeclared for this candidate. Move, Iceberg Move and MassCancel are also undeclared.
The local publisher admission cap defaults to 30 attempts per rolling second and is bounded to 1..3000; a denied or ambiguous command is never automatically retried.
During recovery, effective readiness and Add authority are false. A nonterminal order epoch is preserved as uncertain, with no automatic Add, Cancel, flatten, or compensating command.

The fixed profile routes only the declared SPECTRA/FORTS subsystem and exposes no uncontrolled subsystem selector.
The connector is proprietary software used through a broker account; it is not a broker system offered to clients,
so the broker-system administration/monitoring requirement is explicitly `N/A_PRODUCT_SCOPE`.

`FORTS_REFDATA_REPL.sys_messages` is part of the existing REFDATA subscription. Committed rows preserve message
identity, time, language, urgency, status, text and body, with LifeNum/ClearDeleted provenance. They are exposed by
the explicit qualification snapshot and `moexctl qualify` JSON; malformed optional content cannot change readiness.
The next full-day evidence must also include a nonempty CGate `app_name`, its publisher/reply identity match, and a
clock gate with synchronization source/status, wall offset, monotonic clock identity and paired local/exchange
timestamps. Every paired wall timestamp must be within one second; missing or failing evidence prevents PASS.
The operator response for unexpected orders, ambiguous replies, router/network loss, application/TCS failure or
unrecoverable uncertainty is the [AGGR operator emergency procedure](AGGR_OPERATOR_EMERGENCY_PROCEDURE_9_9.md).

## SPECTRA 9.9 dependency audit

The authoritative consumed-surface lock is [`consumed_replication_compatibility.json`](../../spec-lock/test/plaza2/cgate99/consumed_replication_compatibility.json).
The official 9.9 change list contains additive fields such as `prevorder_id` and `premium_blocked`, and removes the intraday-clearing fields/tables listed below.

| Protocol change | AGGR/private/refdata treatment | Result |
|---|---|---|
| four PART intraday-clearing fields removed | `part` shadows only; `part_sa` is outside AGGR; absent values do not affect identity, limits, readiness, or order authorization | PASS_OFFLINE |
| `session.inter_cl_begin/end/state` removed | retained session members are optional shadows only; session state, current status, trading periods and price terms use the current fields | PASS_OFFLINE |
| three `step_price_interclr` fields removed | unused by AGGR and the price gate; `min_step` and `step_price` remain authoritative | PASS_OFFLINE |
| `fut_exec_orders.xamount_apply`, `opt_exec_orders.xamount_apply` removed | execution-order tables are not opened by the AGGR host and no production path reads these fields | PASS_OFFLINE |
| `fut_intercl_info`, `opt_intercl_info` removed | neither table is in the required private/refdata path; unknown required drift still fails closed | PASS_OFFLINE |
| `prevorder_id` additions | additive and ignored by the AGGR/private gates; no positional or ordinal assumption | PASS_OFFLINE |
| `premium_blocked` addition | additive PART field; participant identity and `limits_set` remain separate | PASS_OFFLINE |

One production-path cleanup was made in this branch: the order safety `limit_row_fingerprint` no longer includes the two removed PART shadow values.
The compatibility members remain only to preserve existing ABI and historical SPECTRA93 fixtures.
The runtime compatibility checker continues to expose reviewed removals as warnings while treating unknown required changes as incompatible.

The existing SPECTRA 9.9 runtime-lock test constructs a scheme with all six reviewed absent fields and requires
`CompatibleWithWarnings` with zero fatal drift. The private-state provenance test covers exact
`2077/184/184 -> lower 1893 / upper 2261`, fixed-scale arithmetic, nulls, stale source revisions,
overflow, and no absolute-value/sign inference. These tests are retained and run by the complete Plaza II suite.

## Evidence already available

The fresh 2026-09-14 order-free observation used source `78f1dded…` and the binary hash above.
It discovered and committed `ALRS-9.26`, `isin_id=4519447`, session `11705`, with current REFDATA,
exact PART identity, `limits_set=true`, zero starting position, zero active own orders, AGGR BBO `1971/1974`
with age 1 ms, and all effective readiness flags true. No publisher message was posted and the process exited cleanly.
The immutable evidence remains under `.codex-tmp/merged-main-20260914/` and is not rewritten.

That result is observation evidence only. It does not prove an Add/Working/Cancel/Cancelled lifecycle,
a full-day session timeline, a restart with a Working order, or a live router/transport loss on this candidate.
Earlier live failures and passes retain their original source, binary, runtime, scheme, and scenario identities.

## Next live gate

When T1 is open, follow the [current runbook](AGGR_T1_QUALIFICATION_9_9.md): refresh availability and discover the current session,
run the independent 300-second idle probe, then start the persistent host and record fresh source/binary/runtime/scheme/config hashes.
After all current safety gates pass, run the one-lot `FIRST_ORDER_DEEP_PASSIVE_V1` lifecycle:

```text
AddOrder 474 -> {business reply 179 + exact matching private Working}
    -> immediate DelOrder 461
    -> {business reply 177 + exact matching private Cancelled + zero active orders}
    -> final position reconciliation
```

Each brace group is a conjunction, not a cross-channel arrival-order requirement; either may arrive first across the two channels.
Working may precede 179 or 179 may precede Working; Cancelled may precede 177 or 177 may precede Cancelled. DelOrder is sent only after
the Add conjunction agrees. System replies 99/100 are decoded and reconciled, never promoted to ordinary success.

Then run the separately gated zero-order restart, one-Working-order process restart, local-router recovery,
the C05-C07 client-controlled upstream-Plaza fault equivalent, and only then any MOEX-coordinated
upstream/TCS/schema/access exercise that remains technically unavoidable. For C05-C07, keep the dedicated
local router running and block only its outbound MOEX destination traffic; discover endpoints without
printing credentials, schedule automatic rollback before the rule, preserve the original network state,
and verify rollback after each case. If safe upstream-only injection cannot be demonstrated, preserve the
offline result and classify only that live portion `MOEX_COORDINATED` with the recorded reason.
Freeze a certification candidate only after those receipts, full Release/sanitizer/Linux/LSan validation, and the matrix review are complete.

Until then the correct final verdict remains **AGGR_NOT_READY_FOR_MOEX_CERTIFICATION**.
