#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path


REMOVED_FIELDS = {
    "FORTS_PART_REPL.part.vm_intercl",
    "FORTS_PART_REPL.part.premium_intercl",
    "FORTS_REFDATA_REPL.fut_instruments.step_price_interclr",
    "FORTS_REFDATA_REPL.session.inter_cl_begin",
    "FORTS_REFDATA_REPL.session.inter_cl_end",
    "FORTS_REFDATA_REPL.session.inter_cl_state",
}

OFFICIAL_REMOVED_SURFACES = {
    *REMOVED_FIELDS,
    "FORTS_PART_REPL.part_sa.vm_intercl",
    "FORTS_PART_REPL.part_sa.premium_intercl",
    "FORTS_REFDATA_REPL.sess_option_series.step_price_interclr",
    "FORTS_REFDATA_REPL.fut_sess_contents.step_price_interclr",
    "FORTS_REFDATA_REPL.fut_exec_orders.xamount_apply",
    "FORTS_REFDATA_REPL.opt_exec_orders.xamount_apply",
    "FORTS_REFDATA_REPL.fut_intercl_info",
    "FORTS_REFDATA_REPL.opt_intercl_info",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8")


def read_locked_commands(path: Path) -> dict[str, dict[str, object]]:
    rows: dict[str, dict[str, object]] = {}
    pattern = re.compile(
        r"- \{name: ([A-Za-z0-9_]+), msgid: (\d+), replies: \[([^]]+)\], payload_size: (\d+)\}"
    )
    for match in pattern.finditer(path.read_text(encoding="utf-8")):
        name, msgid, replies, payload_size = match.groups()
        rows[name] = {
            "name": name,
            "msgid": int(msgid),
            "replies": [int(value.strip()) for value in replies.split(",")],
            "payload_size": int(payload_size),
        }
    return rows


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: assert_plaza2_aggr_9_9_manifest.py <repo-root>")
    root = Path(sys.argv[1])
    manifest_path = root / "cert/aggr_plaza2_certification_manifest_9_9.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    require(manifest["schema"] == "moex.plaza2.aggr-certification-manifest.v1", "manifest schema changed")
    require(
        manifest["source_base_sha"] == "78f1dded089453d8e3d52a3f1fc26536baf1b197",
        "manifest source base is not the reviewed merged main",
    )
    try:
        subprocess.run(
            ["git", "merge-base", "--is-ancestor", manifest["source_base_sha"], "HEAD"],
            cwd=root,
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise AssertionError("current checkout is not a descendant of the reviewed source base") from error

    runtime = manifest["runtime_lock"]
    require(runtime["release"] == "SPECTRA9.9.0", "active release is not SPECTRA 9.9")
    require(runtime["compatibility"] == "CompatibleWithWarnings", "runtime compatibility policy changed")
    require(runtime["fatal_drift_count"] == 0, "current runtime lock has fatal drift")
    require(
        runtime["scheme_sha256"] == "7b93117ee435fd0cb2849b677fc32a9d581364b6ee9afeac9c6c002875400746",
        "active scheme hash changed without a new lock",
    )
    require(
        runtime["library_sha256"] == "f63e726a8482b793c3af755a8dc2b9ebb5cd727d88fb58ebb3fe9704a155ce6f",
        "active CGate library hash changed without a new lock",
    )

    topology = manifest["topology"]
    private = {entry["stream"] for entry in topology["private_replication"]}
    require(
        private == {
            "FORTS_POS_REPL",
            "FORTS_PART_REPL",
            "FORTS_TRADE_REPL",
            "FORTS_USERORDERBOOK_REPL",
            "FORTS_REFDATA_REPL",
        },
        "private stream set is not the exact AGGR host set",
    )
    require(
        {entry["stream"] for entry in topology["status_replication"]}
        == {"FORTS_SESSIONSTATE_REPL", "FORTS_INSTRUMENTSTATE_REPL"},
        "status stream set changed",
    )
    require(topology["aggregated_replication"]["stream"] == "FORTS_AGGR20_REPL", "AGGR stream changed")
    require("FORTS_ORDLOG_REPL" not in private, "public ORDLOG entered the AGGR host")
    require("FORTS_ORDBOOK_REPL" not in private, "public ORDBOOK entered the AGGR host")

    commands = manifest["command_surface"]["declared"]
    require(
        {
            (
                command["name"],
                command["command_id"],
                command["business_reply_id"],
                tuple(command["system_reply_ids"]),
            )
            for command in commands
        }
        == {
            ("AddOrder", 474, 179, (99, 100)),
            ("DelOrder", 461, 177, (99, 100)),
        },
        "declared command/business/system reply surface changed",
    )
    require(
        manifest["command_surface"]["known_not_declared"]
        == [
            {
                "name": "DelUserOrders",
                "command_id": 466,
                "business_reply_id": 186,
                "system_reply_ids": [99, 100],
            }
        ],
        "known undeclared DelUserOrders reply mapping changed",
    )
    require(
        manifest["command_surface"]["reply_policy"]
        == "business: AddOrder=179; DelOrder=177; system: 99/100; correlation never implies ordinary success",
        "reply policy must keep system replies out of ordinary success",
    )
    require(
        manifest["command_surface"]["ordinary_lifecycle"]
        == [
            "AddOrder 474",
            "both: correlated business reply 179 + exact matching private Working evidence (either may arrive first)",
            "DelOrder 461 only after both Add facts agree",
            "both: correlated business reply 177 + exact private Cancelled evidence + zero active own orders (either may arrive first)",
            "reconcile final position",
        ],
        "ordinary lifecycle reply semantics changed",
    )
    require(
        "independent" in manifest["command_surface"]["asynchronous_channel_policy"]
        and "either may arrive first" in manifest["command_surface"]["asynchronous_channel_policy"],
        "asynchronous reply/private policy is missing",
    )
    certification_authority = manifest["official_sources"]["certification_authority"]
    require(
        certification_authority
        == {
            "url": "https://www.moex.com/files/4xgv6e2x1paqr1zkn2fmq093cj",
            "title": "ПОРЯДОК СЕРТИФИКАЦИИ ВНЕШНИХ ПРОГРАММНО-ТЕХНИЧЕСКИХ СРЕДСТВ (ВПТС) ПАО МОСКОВСКАЯ БИРЖА",
            "effective_or_approved_date": "2023-01-30",
            "retrieved_date": "2026-09-14",
            "downloaded_document_sha256": "91c24dd5d03947e03f4d2b9fa3a78ab1b4b9d5c8fc43dc91e9e7205135caf2f1",
            "section_used": "Appendix 1 Plaza II (Connection 1-8, Replication 1-8, Sending 1-7) and general requirements on pp. 2-4",
            "authority_note": (
                "Approved by MOEX order МБ-П-2023-207 dated 30.01.2023; internal C/R/S labels remain one-to-one "
                "traceability IDs, not MOEX identifiers."
            ),
        },
        "certification authority pin changed",
    )
    vpts = manifest["official_sources"]["vpts_technical_requirements"]
    require(
        vpts
        == {
            "url": "https://www.moex.com/files/41w8g1tt63pd9tq9drmk4n3g4z",
            "title": "Порядок сертификации ВПТС ПАО Московская Биржа — текущие Требования к сопряжению с ПТК ТЦ",
            "effective_or_current_edition_date": "2020-08-17",
            "date_basis": "Current edition effective 17.08.2020; approved by MOEX order МБ-П-2020-1938 dated 07.08.2020",
            "retrieved_date": "2026-09-14",
            "downloaded_document_sha256": "a775f5c5aca3cefba58498549d8ff076055091faea757a6a0fbf1ba848f4a4a2",
            "section_used": "Section 2.1-2.12: LifeNum/ClearDeleted; admin; app identity; clock; Exchange/NCC; monitoring; redundancy",
        },
        "VPTS authority pin changed",
    )
    locked_trade = read_locked_commands(root / "spec-lock/test/plaza2/trade/SPECTRA9.9.0/manifest.yaml")
    locked_commands = {name: row for name, row in locked_trade.items() if name in {"AddOrder", "DelOrder", "DelUserOrders"}}
    require(
        locked_commands
        == {
            "AddOrder": {"name": "AddOrder", "msgid": 474, "replies": [179, 99, 100], "payload_size": 112},
            "DelOrder": {"name": "DelOrder", "msgid": 461, "replies": [177, 99, 100], "payload_size": 20},
            "DelUserOrders": {"name": "DelUserOrders", "msgid": 466, "replies": [186, 99, 100], "payload_size": 49},
        },
        "SPECTRA 9.9 transactional lock mappings changed",
    )
    require(
        manifest["command_surface"]["declared"]
        == [
            {"name": "AddOrder", "command_id": 474, "business_reply_id": 179, "system_reply_ids": [99, 100]},
            {"name": "DelOrder", "command_id": 461, "business_reply_id": 177, "system_reply_ids": [99, 100]},
        ],
        "manifest declared commands do not match the locked mappings",
    )
    require("MoveOrder" in manifest["command_surface"]["not_declared_for_this_candidate"], "MoveOrder claim widened")
    require("MassCancel" in manifest["command_surface"]["not_declared_for_this_candidate"], "MassCancel claim widened")
    require(manifest["official_matrix"]["appendix1_equivalence"] == {
        "result": "EQUIVALENT_CONTROLS_NO_IMPLEMENTATION_CHANGE",
        "source_control_groups": ["Connection 1-8 -> C01-C08", "Replication 1-8 -> R01-R08", "Sending 1-7 -> S01-S07"],
        "basis": (
            "Current 2023 Appendix 1 groups and numbering are equivalent to the prior mapping; "
            "wording re-audited; no implementation remap required."
        ),
    }, "Appendix 1 equivalence audit changed")
    require(manifest["clock_gate"]["max_skew_ns"] == 1_000_000_000, "clock gate limit changed")
    require(manifest["clock_gate"]["t1_status"] == "NOT_RUN_T1_SESSION_CLOSED", "closed-session clock status changed")
    require(manifest["instance_identity"]["field"] == "app_name", "instance identity field changed")
    require(manifest["system_messages"]["table"] == "FORTS_REFDATA_REPL.sys_messages", "system-message table changed")
    require((root / manifest["operator_emergency_procedure"]).is_file(), "operator emergency procedure missing")

    compatibility = json.loads(
        (root / "spec-lock/test/plaza2/cgate99/consumed_replication_compatibility.json").read_text(encoding="utf-8")
    )
    require(set(compatibility["reviewed_absent_fields"]) == REMOVED_FIELDS, "9.9 removal set drifted")
    require(set(manifest["spectra_9_9_audit"]["reviewed_absent_fields"]) == REMOVED_FIELDS, "manifest removal set drifted")
    require(
        set(manifest["spectra_9_9_audit"]["official_removed_surfaces"]) == OFFICIAL_REMOVED_SURFACES,
        "official 9.9 removal set is incomplete or changed",
    )

    # Removed names may exist in generated schemas and in the explicit reviewed
    # compatibility table. They must not enter the AGGR hot path or the order
    # safety fingerprint. Private-state shadow parsing is intentionally allowed
    # once per field to keep the existing ABI and old fixtures intact.
    active_paths = (
        "protocols/plaza2_cgate/src/plaza2_aggr20_md.cpp",
        "connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp",
        "connectors/connector_host/src/operator_config.cpp",
    )
    for relative in active_paths:
        text = read(root, relative)
        for removed in OFFICIAL_REMOVED_SURFACES:
            short = removed.rsplit(".", 1)[-1]
            require(short not in text, f"removed 9.9 field entered active path {relative}: {short}")
    private_source = read(root, "protocols/plaza2_cgate/src/plaza2_private_state.cpp")
    allowed_shadow_reads = {
        "kFortsPartReplPartVmIntercl": 1,
        "kFortsPartReplPartPremiumIntercl": 1,
        "kFortsRefdataReplSessionInterClBegin": 1,
        "kFortsRefdataReplSessionInterClEnd": 1,
        "kFortsRefdataReplSessionInterClState": 1,
    }
    for field, count in allowed_shadow_reads.items():
        require(private_source.count(field) == count, f"unexpected SPECTRA93 shadow read count: {field}")
    fingerprint_source = read(root, "connectors/plaza2_trade/src/plaza2_test_trade_transport.cpp")
    require("limit.vm_intercl" not in fingerprint_source, "removed vm_intercl is in safety fingerprint")
    require("limit.premium_intercl" not in fingerprint_source, "removed premium_intercl is in safety fingerprint")

    terms_source = read(root, "protocols/plaza2_cgate/src/plaza2_private_state.cpp")
    require("reference + up" in terms_source and "reference - down" in terms_source, "price formula changed")
    require("abs(" not in terms_source, "price formula uses absolute-value inference")
    require("settlement_price_open" not in terms_source, "price formula uses settlement_price_open")
    provenance_test = read(root, "tests/plaza2_cgate/plaza2_private_state_provenance_test.cpp")
    for needle in ("2077", "1893", "2261", "addition overflow", "subtraction underflow", "changed REFDATA LifeNum"):
        require(needle in provenance_test, f"price/provenance regression coverage missing: {needle}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"AGGR 9.9 manifest check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
