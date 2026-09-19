#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import sys
from pathlib import Path


OFFICIAL_ROWS = {
    "C01": "connection URLs",
    "C02": "connection/thread ownership",
    "C03": ">=300-second idle polling without losing router connection",
    "C04": "connection to authenticated router",
    "C05": "router available while Plaza network is unavailable; wait/no-send",
    "C06": "detect Plaza network becoming available",
    "C07": "detect Plaza network loss and stop sends/stream use until recovered",
    "C08": "detect router connection loss and stop activity",
    "R01": "subscription URLs",
    "R02": "subscription/thread ownership",
    "R03": "correct client receive scheme where applicable",
    "R04": "compatible server-scheme additions",
    "R05": "incompatible removal/type-change detection",
    "R06": "loss and correct reopening of every declared stream",
    "R07": ">=100k msg/s Full ORDERS_LOG",
    "R08": "ClearDeleted and LifeNum",
    "S01": "publisher URLs",
    "S02": "publisher/thread ownership",
    "S03": "configurable rate control",
    "S04": "correct send scheme",
    "S05": "replies and timeouts handled without undefined state",
    "S06": "reply types 99 and 100 handled correctly",
    "S07": "publisher-loss detection and correct reopening",
}

GENERAL_ROWS = {
    "complete interaction logs",
    "network interruption recovery",
    "application restart during the trading day",
    "TCS restart with reload",
    "TCS restart without reload",
    "reserve/access-server switching",
    "full SPECTRA trading-day exercise with all declared command types",
    "administrator/emergency procedure",
    "per-instance customer-software identifier",
    "log/system-time ±1 sec",
    "Exchange/NCC messages",
    "administration/monitoring for broker systems",
    "one-to-one MOEX terminology",
    "fixed SPECTRA subsystem routing",
    "broker-system/client-operation applicability",
}

ALLOWED_CLASSIFICATIONS = {
    "AGGR_REQUIRED",
    "MOEX_COORDINATED",
    "N/A_CLIENT_SCHEME",
    "N/A_PRODUCT_SCOPE",
    "N/A_FIXED_SPECTRA_PROFILE",
    "DEFERRED_FULL_ORDLOG_PHASE",
}
ALLOWED_OFFLINE = {"PASS_OFFLINE", "DEFERRED_FULL_ORDLOG_PHASE"}
ALLOWED_T1 = {
    "PASS_T1",
    "NOT_RUN_T1_SESSION_CLOSED",
    "NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED",
    "MOEX_COORDINATED",
    "N/A_CLIENT_SCHEME",
    "N/A_PRODUCT_SCOPE",
    "N/A_FIXED_SPECTRA_PROFILE",
    "DEFERRED_FULL_ORDLOG_PHASE",
}
FIELD_RE = re.compile(r"^- (Classification|Offline result|T1 result|Code/test evidence|Exact T1 evidence|Remaining action): (.+)$")
HEADING_RE = re.compile(r"^### (C\d\d|R\d\d|S\d\d|General) — (.+)$")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def parse_rows(lines: list[str]) -> dict[str, dict[str, str]]:
    rows: dict[str, dict[str, str]] = {}
    current_id: str | None = None
    for line in lines:
        heading = HEADING_RE.match(line)
        if heading:
            prefix, requirement = heading.groups()
            identifier = prefix if prefix != "General" else f"General — {requirement}"
            require(identifier not in rows, f"duplicate matrix heading: {identifier}")
            rows[identifier] = {"Official requirement": requirement}
            current_id = identifier
            continue
        field = FIELD_RE.match(line)
        if field and current_id is not None:
            name, value = field.groups()
            require(name not in rows[current_id], f"duplicate field {name}: {current_id}")
            rows[current_id][name] = value
    return rows


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: assert_plaza2_aggr_9_9_matrix.py <repo-root>")
    root = Path(sys.argv[1]).resolve()
    matrix_path = root / "cert/AGGR_CERT_MATRIX_9_9.md"
    matrix = matrix_path.read_text(encoding="utf-8")
    require("P2-C" not in matrix and "P2-R" not in matrix and "P2-S" not in matrix, "official IDs were repurposed")
    require(
        "AddOrder 474 -> {business reply 179 + matching private Working} -> DelOrder 461" in matrix
        and "{business reply 177 + matching private Cancelled + zero active orders}" in matrix,
        "ordinary lifecycle conjunction semantics missing from matrix",
    )
    require("system replies 99/100 are decoded" in matrix.lower(), "system reply policy missing from matrix")
    require("internal stable" in matrix and "MOEX does not name" in matrix, "authority provenance wording is missing")
    require("2023-01-30" in matrix and "91c24dd5d03947e03f4d2b9fa3a78ab1b4b9d5c8fc43dc91e9e7205135caf2f1" in matrix,
            "current 2023 certification procedure pin is missing")
    require("EQUIVALENT_CONTROLS_NO_IMPLEMENTATION_CHANGE" in matrix or "no implementation remap" in matrix,
            "Appendix 1 equivalence decision is missing")

    rows = parse_rows(matrix.splitlines())
    official = {key: value for key, value in rows.items() if key in OFFICIAL_ROWS}
    general = {key.split(" — ", 1)[1]: value for key, value in rows.items() if key.startswith("General — ")}
    require(set(official) == set(OFFICIAL_ROWS), "official C/R/S row set is incomplete or has extra IDs")
    require(set(general) == GENERAL_ROWS, "general requirement set is incomplete or changed")

    for identifier, requirement in OFFICIAL_ROWS.items():
        row = official[identifier]
        require(row["Official requirement"] == requirement, f"{identifier} requirement was changed")
        require(row["Classification"] in ALLOWED_CLASSIFICATIONS, f"{identifier} classification is invalid")
        require(row["Offline result"] in ALLOWED_OFFLINE, f"{identifier} offline result is invalid")
        require(row["T1 result"] in ALLOWED_T1, f"{identifier} T1 result is invalid")
        for field in ("Code/test evidence", "Exact T1 evidence", "Remaining action"):
            require(row.get(field), f"{identifier} lacks traceability field {field}")

    for requirement, row in general.items():
        require(row["Classification"] in ALLOWED_CLASSIFICATIONS, f"general classification is invalid: {requirement}")
        require(row["Offline result"] in ALLOWED_OFFLINE, f"general offline result is invalid: {requirement}")
        require(row["T1 result"] in ALLOWED_T1, f"general T1 result is invalid: {requirement}")
        for field in ("Code/test evidence", "Exact T1 evidence", "Remaining action"):
            require(row.get(field), f"general row lacks traceability field {field}")

    r07 = official["R07"]
    require(r07["Classification"] == "DEFERRED_FULL_ORDLOG_PHASE", "R07 classification must remain deferred")
    require(r07["Offline result"] == "DEFERRED_FULL_ORDLOG_PHASE", "R07 offline result must remain deferred")
    require(r07["T1 result"] == "DEFERRED_FULL_ORDLOG_PHASE", "R07 T1 result must remain deferred")
    for identifier in ("C05", "C06", "C07"):
        require(
            official[identifier]["Classification"] == "AGGR_REQUIRED",
            f"{identifier} must remain an AGGR-required behavior",
        )
        require(
            official[identifier]["T1 result"] == "NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED",
            f"{identifier} must remain pending until a safe client fault attempt",
        )
    for identifier in ("R04", "R05"):
        require(
            official[identifier]["Classification"] == "N/A_CLIENT_SCHEME"
            and official[identifier]["T1 result"] == "N/A_CLIENT_SCHEME",
            f"{identifier} must be explicitly N/A_CLIENT_SCHEME for the client-scheme profile",
        )
    require(
        official["C08"]["T1 result"] == "NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED",
        "C08 local-router test must remain a pending T1 gate",
    )

    manifest = json.loads((root / "cert/aggr_plaza2_certification_manifest_9_9.json").read_text(encoding="utf-8"))
    authority = manifest["official_sources"]["certification_authority"]
    require(
        "internal stable IDs" in manifest["official_matrix"]["authority"] and
        "one-to-one" in manifest["official_matrix"]["authority"],
        "manifest must describe C/R/S labels as internal stable IDs",
    )
    require(
        manifest["official_matrix"]["authority_source"] == "official_sources.certification_authority",
        "matrix must point to the pinned certification authority",
    )
    for key in (
        "url",
        "title",
        "effective_or_approved_date",
        "retrieved_date",
        "downloaded_document_sha256",
        "section_used",
    ):
        require(authority.get(key), f"certification authority pin lacks {key}")
    vpts = manifest["official_sources"]["vpts_technical_requirements"]
    for key in (
        "url",
        "title",
        "effective_or_current_edition_date",
        "retrieved_date",
        "downloaded_document_sha256",
        "section_used",
    ):
        require(vpts.get(key), f"VPTS authority pin lacks {key}")
    require(authority["effective_or_approved_date"] == "2023-01-30", "matrix uses an obsolete procedure edition")
    require(authority["downloaded_document_sha256"] == "91c24dd5d03947e03f4d2b9fa3a78ab1b4b9d5c8fc43dc91e9e7205135caf2f1",
            "current procedure hash changed")
    require(vpts["effective_or_current_edition_date"] == "2020-08-17", "matrix uses an obsolete VPTS edition")
    require(vpts["downloaded_document_sha256"] == "a775f5c5aca3cefba58498549d8ff076055091faea757a6a0fbf1ba848f4a4a2",
            "current VPTS requirements hash changed")
    require(manifest["official_matrix"]["appendix1_equivalence"]["result"] == "EQUIVALENT_CONTROLS_NO_IMPLEMENTATION_CHANGE",
            "manifest Appendix 1 equivalence decision missing")
    require((root / manifest["operator_emergency_procedure"]).is_file(), "operator emergency procedure is missing")
    require(manifest["clock_gate"]["max_skew_ns"] == 1_000_000_000 and
            manifest["clock_gate"]["t1_status"] == "NOT_RUN_T1_SESSION_CLOSED", "clock gate is incomplete")
    require(manifest["system_messages"]["table"] == "FORTS_REFDATA_REPL.sys_messages" and
            "QualificationSnapshot" in manifest["system_messages"]["surface"], "system-message gate is incomplete")
    require(manifest["instance_identity"]["field"] == "app_name" and
            manifest["instance_identity"]["required"] == "nonempty for every live TEST connection", "instance identity gate is incomplete")

    report = (root / "docs/plaza2/AGGR_CERTIFICATION_9_9.md").read_text(encoding="utf-8")
    runbook = (root / "docs/plaza2/AGGR_T1_QUALIFICATION_9_9.md").read_text(encoding="utf-8")
    for relative, text in {
        "docs/plaza2/AGGR_CERTIFICATION_9_9.md": report,
        "docs/plaza2/AGGR_T1_QUALIFICATION_9_9.md": runbook,
    }.items():
        require("business reply 179" in text and "business reply 177" in text, f"{relative} lacks business reply semantics")
        require("either may arrive first" in text and "conjunction" in text, f"{relative} lacks asynchronous conjunction policy")
        require("system replies 99/100" in text or "Reply 99 or 100" in text, f"{relative} lacks system reply policy")
        require("app_name" in text and "one second" in text, f"{relative} lacks instance/clock gate")
        require("sys_messages" in text and "emergency" in text.lower(), f"{relative} lacks message/emergency gate")
        require("accepted reply 99" not in text.lower(), f"{relative} promotes reply 99 to ordinary success")
        require("reply 100 -> exact private Cancelled" not in text, f"{relative} promotes reply 100 to ordinary success")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"AGGR 9.9 matrix-authority check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
