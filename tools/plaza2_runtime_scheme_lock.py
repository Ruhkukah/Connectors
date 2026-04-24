#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


REQUIRED_TABLES = {
    ("FORTS_REFDATA_REPL", "session"),
    ("FORTS_REFDATA_REPL", "fut_instruments"),
    ("FORTS_REFDATA_REPL", "opt_sess_contents"),
    ("FORTS_REFDATA_REPL", "multileg_dict"),
    ("FORTS_REFDATA_REPL", "instr2matching_map"),
    ("FORTS_TRADE_REPL", "orders_log"),
    ("FORTS_TRADE_REPL", "multileg_orders_log"),
    ("FORTS_TRADE_REPL", "user_deal"),
    ("FORTS_TRADE_REPL", "user_multileg_deal"),
    ("FORTS_TRADE_REPL", "heartbeat"),
    ("FORTS_TRADE_REPL", "sys_events"),
    ("FORTS_USERORDERBOOK_REPL", "orders"),
    ("FORTS_USERORDERBOOK_REPL", "multileg_orders"),
    ("FORTS_USERORDERBOOK_REPL", "orders_currentday"),
    ("FORTS_USERORDERBOOK_REPL", "multileg_orders_currentday"),
    ("FORTS_USERORDERBOOK_REPL", "info"),
    ("FORTS_USERORDERBOOK_REPL", "info_currentday"),
    ("FORTS_POS_REPL", "position"),
    ("FORTS_POS_REPL", "info"),
    ("FORTS_PART_REPL", "part"),
    ("FORTS_PART_REPL", "sys_events"),
}

WARNING_ONLY_TABLES = {
    ("FORTS_REFDATA_REPL", "clearing_members"),
}

RUNTIME_SCHEME_ALIASES = {
    "REFDATA": "FORTS_REFDATA_REPL",
    "Trade": "FORTS_TRADE_REPL",
    "OrderBook": "FORTS_USERORDERBOOK_REPL",
    "POS": "FORTS_POS_REPL",
    "PART": "FORTS_PART_REPL",
}


def ensure_dir(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=True)
    return path


def dump_json(data: object, path: Path) -> None:
    ensure_dir(path.parent)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_runtime_scheme(path: Path) -> dict:
    markers: dict[str, str] = {}
    tables: dict[tuple[str, str], list[dict[str, str]]] = {}
    current: tuple[str, str] | None = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith(";"):
            comment = line[1:].strip()
            for label, key in (
                ("Spectra release:", "spectra_release"),
                ("DDS version:", "dds_version"),
                ("Target polygon:", "target_polygon"),
                ("Target poligon:", "target_polygon"),
            ):
                if comment.startswith(label):
                    markers[key] = comment[len(label) :].strip()
            continue
        if line.startswith("[table:") and line.endswith("]"):
            body = line[len("[table:") : -1]
            stream, sep, table = body.partition(":")
            if not sep or not stream or not table:
                raise ValueError(f"invalid table section: {line}")
            current = (RUNTIME_SCHEME_ALIASES.get(stream, stream), table)
            tables.setdefault(current, [])
            continue
        if line.startswith("["):
            current = None
            continue
        if current is None or not line.startswith("field="):
            continue
        body = line[len("field=") :]
        name, sep, type_token = body.partition(",")
        if not sep or not name.strip() or not type_token.strip():
            raise ValueError(f"invalid field line: {line}")
        tables[current].append({"field_name": name.strip(), "type_token": type_token.strip()})
    return {"markers": markers, "tables": tables}


def reviewed_tables(metadata_path: Path) -> dict[tuple[str, str], list[dict[str, str]]]:
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    tables: dict[tuple[str, str], list[dict[str, str]]] = {
        (row["stream_name"], row["table_name"]): [] for row in metadata["tables"]
    }
    for field in metadata["fields"]:
        key = (field["stream_name"], field["table_name"])
        tables.setdefault(key, []).append({"field_name": field["field_name"], "type_token": field["type_token"]})
    return tables


def table_signature_hash(fields: list[dict[str, str]]) -> str:
    payload = "".join(f"{field['field_name']}:{field['type_token']}\n" for field in fields)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def integer_type_size(type_token: str) -> int | None:
    if type_token in {"i1", "u1"}:
        return 1
    if type_token in {"i2", "u2"}:
        return 2
    if type_token in {"i4", "u4"}:
        return 4
    if type_token in {"i8", "u8"}:
        return 8
    return None


def compatible_type(reviewed_type: str, runtime_type: str) -> bool:
    if reviewed_type == runtime_type:
        return True
    reviewed_size = integer_type_size(reviewed_type)
    runtime_size = integer_type_size(runtime_type)
    return reviewed_size is not None and reviewed_size == runtime_size


def material_required_drift(
    key: tuple[str, str], runtime_fields: list[dict[str, str]], reviewed_fields: list[dict[str, str]]
) -> bool:
    if key not in REQUIRED_TABLES:
        return False
    runtime_by_name = {field["field_name"]: field["type_token"] for field in runtime_fields}
    for field in reviewed_fields:
        runtime_type = runtime_by_name.get(field["field_name"])
        if runtime_type is None or not compatible_type(field["type_token"], runtime_type):
            return True
    return False


def classification(key: tuple[str, str]) -> str:
    if key in REQUIRED_TABLES:
        return "required"
    if key in WARNING_ONLY_TABLES:
        return "warning_only"
    return "ignored_deferred"


def compatibility(fatal: list[dict], warning: list[dict]) -> str:
    if fatal:
        return "Incompatible"
    if warning:
        return "CompatibleWithWarnings"
    return "Compatible"


def build_reports(runtime_scheme: Path, metadata_path: Path, report_path: str | None) -> tuple[dict, dict, str]:
    runtime = parse_runtime_scheme(runtime_scheme)
    runtime_tables = runtime["tables"]
    reviewed = reviewed_tables(metadata_path)

    signature_rows = []
    fatal_drift = []
    warning_drift = []
    ignored_drift = []
    keys = sorted(set(runtime_tables) | set(reviewed))
    for key in keys:
        runtime_fields = runtime_tables.get(key, [])
        reviewed_fields = reviewed.get(key, [])
        runtime_hash = table_signature_hash(runtime_fields) if runtime_fields else ""
        reviewed_hash = table_signature_hash(reviewed_fields) if reviewed_fields else ""
        drift = runtime_hash != reviewed_hash
        row = {
            "stream_name": key[0],
            "table_name": key[1],
            "classification": classification(key),
            "runtime_field_count": len(runtime_fields),
            "reviewed_field_count": len(reviewed_fields),
            "runtime_signature_hash": runtime_hash,
            "reviewed_signature_hash": reviewed_hash,
            "drift": drift,
        }
        signature_rows.append(row)
        if not drift:
            continue
        target = ignored_drift
        if material_required_drift(key, runtime_fields, reviewed_fields):
            target = fatal_drift
        elif row["classification"] == "warning_only":
            target = warning_drift
        target.append(row)

    status = compatibility(fatal_drift, warning_drift + ignored_drift)
    signature = {
        "generated_by": "Generated by tools/plaza2_runtime_scheme_lock.py; derived report, raw vendor scheme not committed.",
        "runtime_scheme": {
            "path": report_path or str(runtime_scheme),
            "sha256": sha256_file(runtime_scheme),
            "markers": runtime["markers"],
        },
        "table_count": len(runtime_tables),
        "field_count": sum(len(fields) for fields in runtime_tables.values()),
        "tables": signature_rows,
    }
    diff = {
        "generated_by": "Generated by tools/plaza2_runtime_scheme_lock.py; derived report, raw vendor scheme not committed.",
        "compatibility": status,
        "runtime_scheme_sha256": sha256_file(runtime_scheme),
        "fatal_drift_count": len(fatal_drift),
        "warning_drift_count": len(warning_drift) + len(ignored_drift),
        "fatal_drift": fatal_drift,
        "warning_drift": warning_drift,
        "ignored_deferred_drift": ignored_drift,
        "required_tables": [f"{stream}.{table}" for stream, table in sorted(REQUIRED_TABLES)],
        "warning_only_tables": [f"{stream}.{table}" for stream, table in sorted(WARNING_ONLY_TABLES)],
    }
    summary = render_summary(signature, diff)
    return signature, diff, summary


def render_summary(signature: dict, diff: dict) -> str:
    markers = signature["runtime_scheme"]["markers"]
    lines = [
        "# PLAZA II Runtime Scheme Lock Summary",
        "",
        "- Generated by `tools/plaza2_runtime_scheme_lock.py`.",
        "- Raw vendor `forts_scheme.ini` is not committed.",
        f"- Runtime scheme SHA256: `{signature['runtime_scheme']['sha256']}`",
        f"- Spectra release: `{markers.get('spectra_release', '')}`",
        f"- Compatibility: `{diff['compatibility']}`",
        f"- Fatal drift count: `{diff['fatal_drift_count']}`",
        f"- Warning drift count: `{diff['warning_drift_count']}`",
        "",
        "## Warning Drift",
    ]
    if diff["warning_drift"] or diff["ignored_deferred_drift"]:
        for row in diff["warning_drift"] + diff["ignored_deferred_drift"]:
            lines.append(f"- `{row['stream_name']}.{row['table_name']}` ({row['classification']})")
    else:
        lines.append("- None.")
    lines.extend(["", "## Fatal Drift"])
    if diff["fatal_drift"]:
        for row in diff["fatal_drift"]:
            lines.append(f"- `{row['stream_name']}.{row['table_name']}`")
    else:
        lines.append("- None.")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Materialize PLAZA II runtime scheme signature reports.")
    parser.add_argument("--runtime-scheme", required=True, type=Path)
    parser.add_argument("--phase3b-metadata", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--runtime-scheme-report-path", default="")
    args = parser.parse_args()

    signature, diff, summary = build_reports(
        args.runtime_scheme,
        args.phase3b_metadata,
        args.runtime_scheme_report_path or None,
    )
    ensure_dir(args.output)
    dump_json(signature, args.output / "runtime_scheme_signature.json")
    dump_json(diff, args.output / "runtime_vs_reviewed_diff.json")
    (args.output / "runtime_scheme_lock_summary.md").write_text(summary, encoding="utf-8")
    return 0 if diff["compatibility"] != "Incompatible" else 2


if __name__ == "__main__":
    raise SystemExit(main())
