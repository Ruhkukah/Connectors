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
}

ALLOWED_CLASSIFICATIONS = {"AGGR_REQUIRED", "MOEX_COORDINATED", "N/A_CLIENT_SCHEME", "DEFERRED_FULL_ORDLOG_PHASE"}
ALLOWED_OFFLINE = {"PASS_OFFLINE", "DEFERRED_FULL_ORDLOG_PHASE"}
ALLOWED_T1 = {
    "PASS_T1",
    "NOT_RUN_T1_SESSION_CLOSED",
    "MOEX_COORDINATED",
    "N/A_CLIENT_SCHEME",
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
            official[identifier]["T1 result"] == "NOT_RUN_T1_SESSION_CLOSED",
            f"{identifier} must remain pending until a safe client fault attempt",
        )
    for identifier in ("R04", "R05"):
        require(
            official[identifier]["Classification"] == "N/A_CLIENT_SCHEME"
            and official[identifier]["T1 result"] == "N/A_CLIENT_SCHEME",
            f"{identifier} must be explicitly N/A_CLIENT_SCHEME for the client-scheme profile",
        )
    require(official["C08"]["T1 result"] == "NOT_RUN_T1_SESSION_CLOSED", "C08 local-router test must remain a pending T1 gate")

    manifest = json.loads((root / "cert/aggr_plaza2_certification_manifest_9_9.json").read_text(encoding="utf-8"))
    authority = manifest["official_sources"]["certification_authority"]
    require(
        manifest["official_matrix"]["authority"].startswith("internal stable IDs mapped one-to-one"),
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

    report = (root / "docs/plaza2/AGGR_CERTIFICATION_9_9.md").read_text(encoding="utf-8")
    runbook = (root / "docs/plaza2/AGGR_T1_QUALIFICATION_9_9.md").read_text(encoding="utf-8")
    for relative, text in {
        "docs/plaza2/AGGR_CERTIFICATION_9_9.md": report,
        "docs/plaza2/AGGR_T1_QUALIFICATION_9_9.md": runbook,
    }.items():
        require("business reply 179" in text and "business reply 177" in text, f"{relative} lacks business reply semantics")
        require("either may arrive first" in text and "conjunction" in text, f"{relative} lacks asynchronous conjunction policy")
        require("system replies 99/100" in text or "Reply 99 or 100" in text, f"{relative} lacks system reply policy")
        require("accepted reply 99" not in text.lower(), f"{relative} promotes reply 99 to ordinary success")
        require("reply 100 -> exact private Cancelled" not in text, f"{relative} promotes reply 100 to ordinary success")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"AGGR 9.9 matrix-authority check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
