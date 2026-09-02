#!/usr/bin/env python3
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_runtime_scheme(
    metadata: dict,
    path: Path,
    drop_field: tuple[str, str] | None = None,
    spectra_release: str = "SPECTRA93",
    reviewed_absent_fields: set[tuple[str, str, str]] | None = None,
) -> None:
    fields_by_table: dict[tuple[str, str], list[dict]] = {}
    for field in metadata["fields"]:
        fields_by_table.setdefault((field["stream_name"], field["table_name"]), []).append(field)
    lines = [
        f"; Spectra release: {spectra_release}",
        "; DDS version: 93.0.0.0",
        "; Target polygon: test",
        "",
    ]
    for table in metadata["tables"]:
        key = (table["stream_name"], table["table_name"])
        lines.append(f"[table:{key[0]}:{key[1]}]")
        skipped = False
        for field in fields_by_table[key]:
            if reviewed_absent_fields and (key[0], key[1], field["field_name"]) in reviewed_absent_fields:
                continue
            if drop_field == key and not skipped:
                skipped = True
                continue
            lines.append(f"field={field['field_name']},{field['type_token']}")
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def run_tool(root: Path, runtime_scheme: Path, output: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(root / "tools/plaza2_runtime_scheme_lock.py"),
            "--runtime-scheme",
            str(runtime_scheme),
            "--phase3b-metadata",
            str(root / "protocols/plaza2_cgate/generated/plaza2_generated_metadata.json"),
            "--output",
            str(output),
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def assert_report(output: Path, status: str, fatal_count: int) -> dict:
    diff = json.loads((output / "runtime_vs_reviewed_diff.json").read_text(encoding="utf-8"))
    require(diff["compatibility"] == status, f"expected {status}, got {diff['compatibility']}")
    require(diff["fatal_drift_count"] == fatal_count, "fatal drift count mismatch")
    require((output / "runtime_scheme_signature.json").exists(), "signature report missing")
    require((output / "runtime_scheme_lock_summary.md").exists(), "summary report missing")
    return diff


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: assert_plaza2_runtime_scheme_lock.py <repo-root>")
    root = Path(sys.argv[1])
    metadata = json.loads((root / "protocols/plaza2_cgate/generated/plaza2_generated_metadata.json").read_text())
    with tempfile.TemporaryDirectory(prefix="plaza2-runtime-scheme-lock-") as tmp_raw:
        tmp = Path(tmp_raw)

        clean_scheme = tmp / "clean.ini"
        write_runtime_scheme(metadata, clean_scheme)
        clean_run = run_tool(root, clean_scheme, tmp / "clean")
        require(clean_run.returncode == 0, clean_run.stderr)
        assert_report(tmp / "clean", "Compatible", 0)

        warning_scheme = tmp / "warning.ini"
        write_runtime_scheme(metadata, warning_scheme, ("FORTS_REFDATA_REPL", "clearing_members"))
        warning_run = run_tool(root, warning_scheme, tmp / "warning")
        require(warning_run.returncode == 0, warning_run.stderr)
        warning_diff = assert_report(tmp / "warning", "CompatibleWithWarnings", 0)
        require(warning_diff["warning_drift_count"] > 0, "warning drift should be visible")
        require(
            any(row["table_name"] == "clearing_members" for row in warning_diff["warning_drift"]),
            "clearing_members warning drift missing",
        )

        fatal_scheme = tmp / "fatal.ini"
        write_runtime_scheme(metadata, fatal_scheme, ("FORTS_TRADE_REPL", "orders_log"))
        fatal_run = run_tool(root, fatal_scheme, tmp / "fatal")
        require(fatal_run.returncode == 2, "fatal drift should produce incompatible tool exit")
        assert_report(tmp / "fatal", "Incompatible", 1)

        spectra99_scheme = tmp / "spectra99.ini"
        reviewed_absent_fields = {
            ("FORTS_PART_REPL", "part", "vm_intercl"),
            ("FORTS_PART_REPL", "part", "premium_intercl"),
            ("FORTS_REFDATA_REPL", "fut_instruments", "step_price_interclr"),
            ("FORTS_REFDATA_REPL", "session", "inter_cl_begin"),
            ("FORTS_REFDATA_REPL", "session", "inter_cl_end"),
            ("FORTS_REFDATA_REPL", "session", "inter_cl_state"),
        }
        write_runtime_scheme(
            metadata,
            spectra99_scheme,
            spectra_release="SPECTRA9.9.0",
            reviewed_absent_fields=reviewed_absent_fields,
        )
        spectra99_run = run_tool(root, spectra99_scheme, tmp / "spectra99")
        require(spectra99_run.returncode == 0, spectra99_run.stderr)
        spectra99_diff = assert_report(tmp / "spectra99", "CompatibleWithWarnings", 0)
        require(
            all(row["classification"] == "required" for row in spectra99_diff["warning_drift"]),
            "reviewed SPECTRA 9.9 removals should remain visible as required-table warnings",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
