#!/usr/bin/env python3
from __future__ import annotations

import json
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


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8")


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
    require({(command["name"], command["command_id"], command["reply_id"]) for command in commands} == {
        ("AddOrder", 474, 99),
        ("DelOrder", 461, 100),
    }, "declared command/reply surface changed")
    require("MoveOrder" in manifest["command_surface"]["not_declared_for_this_candidate"], "MoveOrder claim widened")
    require("MassCancel" in manifest["command_surface"]["not_declared_for_this_candidate"], "MassCancel claim widened")

    compatibility = json.loads(
        (root / "spec-lock/test/plaza2/cgate99/consumed_replication_compatibility.json").read_text(encoding="utf-8")
    )
    require(set(compatibility["reviewed_absent_fields"]) == REMOVED_FIELDS, "9.9 removal set drifted")
    require(set(manifest["spectra_9_9_audit"]["reviewed_absent_fields"]) == REMOVED_FIELDS, "manifest removal set drifted")

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
        for removed in REMOVED_FIELDS:
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
