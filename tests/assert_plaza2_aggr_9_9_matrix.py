#!/usr/bin/env python3
from __future__ import annotations

import json
import re
import subprocess
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
            official[identifier]["Classification"] == "AGGR_REQUIRED"
            and official[identifier]["T1 result"] == "NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED",
            f"{identifier} applies to the server-scheme status listeners and remains pending for T1",
        )
        require(
            "plaza2_scheme_drift_test" in official[identifier]["Code/test evidence"],
            f"{identifier} must cite the focused server-scheme status fixture",
        )
    for identifier in ("R03", "R04", "R05"):
        require(official[identifier]["Classification"] == "AGGR_REQUIRED",
                f"{identifier} must remain applicable to its configured listener subset")
    require(
        official["C08"]["T1 result"] == "NOT_RUN_T1_SESSION_STATUS_UNCONFIRMED",
        "C08 local-router test must remain a pending T1 gate",
    )

    questionnaire_path = root / "docs/review/moex_cgate_questionnaire_register_9_9_20260919.json"
    questionnaire = json.loads(questionnaire_path.read_text(encoding="utf-8"))
    answers = questionnaire["answers"]
    answer_ids = [row["id"] for row in answers]
    require(len(answers) == 77 and len(set(answer_ids)) == 77,
            "questionnaire register must preserve all 77 unique answer fields")
    stream_answers = [row for row in answers if row["id"].startswith("2a.stream.")]
    require(len(stream_answers) == 33, "questionnaire register must preserve all 33 printed stream checkboxes")
    require(
        questionnaire["source_identity"]["source_head_at_review_start"] ==
            "ffa6552c70bf6b16568ba4f7943c3660019519d2"
        and questionnaire["source_identity"]["ci_head_sha"] ==
            questionnaire["source_identity"]["source_head_at_review_start"]
        and all(value == "SUCCESS" for value in questionnaire["source_identity"]["ci_checks"].values()),
        "questionnaire must bind its CI claim to the exact reviewed source head",
    )
    require(questionnaire["source_identity"]["qualification_binary_sha256"] is None,
            "questionnaire must not invent a deployable Linux binary identity")
    evidence_refs = {ref for row in answers for ref in row["evidence_refs"]}
    require(evidence_refs <= set(questionnaire["evidence_catalog"]), "questionnaire answer has unresolved evidence reference")
    profiles = questionnaire["scheme_policy"]
    read_only_profile = profiles["read_only_dtc_profile"]
    trading_profile = profiles["trading_connector_profile"]
    four = read_only_profile["streams"]
    eight = trading_profile["streams"]
    require(read_only_profile["name"] == "READ_ONLY_DTC_PROFILE" and
            trading_profile["name"] == "TRADING_CONNECTOR_PROFILE",
            "questionnaire must use the canonical names for the two ConnectorHost profiles")
    require({row["service"] for row in four} == {
                "FORTS_REFDATA_REPL", "FORTS_SESSIONSTATE_REPL", "FORTS_INSTRUMENTSTATE_REPL", "FORTS_AGGR20_REPL"
            } and read_only_profile["listener_count"] == 4 and
            not read_only_profile["publisher_configured"] and not read_only_profile["p2mqreply_configured"] and
            not read_only_profile["trade_replay_from_pos_anchor"],
            "READ_ONLY_DTC_PROFILE must describe exactly four read listeners and no trading surface")
    require(len(four) == 4 and
            sum(row["policy"] == "CLIENT_EXPLICIT" for row in four) == 2 and
            sum(row["policy"] == "SERVER_DEFAULT" for row in four) == 2,
            "READ_ONLY_DTC_PROFILE scheme policy must derive as two client/two server listeners")
    require({row["service"] for row in eight} == {
                "FORTS_TRADE_REPL", "FORTS_USERORDERBOOK_REPL", "FORTS_POS_REPL", "FORTS_PART_REPL",
                "FORTS_REFDATA_REPL", "FORTS_SESSIONSTATE_REPL", "FORTS_INSTRUMENTSTATE_REPL", "FORTS_AGGR20_REPL"
            } and len(eight) == 8 and trading_profile["listener_count"] == 8 and
            trading_profile["publisher_configured"] and trading_profile["p2mqreply_configured"] and
            trading_profile["trade_replay_from_pos_anchor"] and
            sum(row["policy"] == "CLIENT_EXPLICIT" for row in eight) == 6 and
            sum(row["policy"] == "SERVER_DEFAULT" for row in eight) == 2,
            "TRADING_CONNECTOR_PROFILE must retain its eight listeners, publisher/reply and 6/2 scheme policy")
    require("apps/plaza2_day_observer" not in " ".join(questionnaire["evidence_catalog"]["profile"]["source_files"])
            and "apps/plaza2_day_observer_profile.hpp" not in
            " ".join(questionnaire["evidence_catalog"]["scheme"]["source_files"]),
            "day observer must not be cited as the DTC runner topology source")
    answer_by_id = {row["id"]: row["answer"] for row in answers}
    require("READ_ONLY_DTC_PROFILE" in answer_by_id["1h"] and
            all("READ_ONLY_DTC_PROFILE" in answer_by_id[identifier] and
                "TRADING_CONNECTOR_PROFILE" in answer_by_id[identifier]
                for identifier in ("2a.i", "2a.ii", "2a.iii", "2a.iv")),
            "1h and 2a connection answers must identify their applicable profile")
    require("TRADING_CONNECTOR_PROFILE" in answer_by_id["2a.stream.22"] and
            "READ_ONLY_DTC_PROFILE" not in answer_by_id["2a.stream.22"] and
            "TRADING_CONNECTOR_PROFILE" in profiles["ordbook_alias_review"]["disposition"] and
            "READ_ONLY_DTC_PROFILE" in profiles["ordbook_alias_review"]["disposition"],
            "OrdBook alias warning must apply only to trading and explicitly exclude read-only")

    signature_path = root / "spec-lock/test/plaza2/runtime_scheme/SPECTRA9.9.0/runtime_scheme_signature.json"
    signature = json.loads(signature_path.read_text(encoding="utf-8"))
    signature_tables: dict[str, set[str]] = {}
    for table in signature["tables"]:
        signature_tables.setdefault(table["stream_name"], set()).add(table["table_name"])
    legacy_names = {"fut_intercl_info", "opt_intercl_info"}
    refdata = next(row for row in questionnaire["stream_inventory"] if row["service"] == "FORTS_REFDATA_REPL")
    require(legacy_names <= signature_tables["FORTS_REFDATA_REPL"],
            "historical scheme lock descriptors must remain present as provenance")
    require(not legacy_names.intersection(refdata["supported_current_scheme_tables"]),
            "removed 9.9 tables must not appear in the current supported inventory")
    require(legacy_names <= set(questionnaire["historical_compatibility_only"]["tables"][i].split(".")[-1]
                                for i in range(len(questionnaire["historical_compatibility_only"]["tables"]))),
            "removed table names must be kept in the separate historical field")
    alias_counts = {
        (row["stream_name"], row["table_name"]): row["runtime_field_count"]
        for row in signature["tables"]
    }
    require(alias_counts[("OrdBook", "orders")] == 17 and
            alias_counts[("FORTS_USERORDERBOOK_REPL", "orders")] == 39 and
            alias_counts[("OrdBook", "multileg_orders")] == 19 and
            alias_counts[("FORTS_USERORDERBOOK_REPL", "multileg_orders")] == 40,
            "questionnaire must preserve the unresolved pinned OrdBook/USERORDERBOOK layout difference")
    require("not establish" in profiles["ordbook_alias_review"]["finding"].lower()
            or "differ" in profiles["ordbook_alias_review"]["finding"].lower()
            or "unproven" in profiles["ordbook_alias_review"]["finding"].lower(),
            "questionnaire must not claim OrdBook layout equivalence")

    rendered = subprocess.run(
        [sys.executable, str(root / "scripts/render_moex_cgate_questionnaire.py"), str(root), "--check"],
        capture_output=True,
        text=True,
        check=False,
    )
    require(rendered.returncode == 0,
            "human-readable questionnaire attachment is stale or missing: " + rendered.stderr.strip())

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
