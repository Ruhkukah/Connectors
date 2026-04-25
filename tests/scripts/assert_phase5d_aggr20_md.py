#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import yaml


def run(command: list[str], *, expect_success: bool) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if expect_success and result.returncode != 0:
        raise AssertionError(f"command failed unexpectedly: {command}\n{result.stderr}")
    if not expect_success and result.returncode == 0:
        raise AssertionError(f"command succeeded unexpectedly: {command}\n{result.stdout}")
    return result


def validate_profile(path: Path) -> None:
    profile = yaml.safe_load(path.read_text(encoding="utf-8"))
    section = profile.get("plaza2_aggr20_md_test") or {}
    stream = section.get("stream") or {}
    if stream.get("name") != "FORTS_AGGR20_REPL":
        raise AssertionError(f"profile {path} does not enable FORTS_AGGR20_REPL")
    settings = str(stream.get("settings", ""))
    if "FORTS_AGGR20_REPL" not in settings:
        raise AssertionError(f"profile {path} stream settings do not use FORTS_AGGR20_REPL")
    for forbidden in ("FORTS_ORDLOG_REPL", "FORTS_ORDBOOK_REPL", "FORTS_DEALS_REPL"):
        if forbidden in settings:
            raise AssertionError(f"profile {path} unexpectedly enables {forbidden}")
    if not profile.get("test_market_data_armed_required"):
        raise AssertionError(f"profile {path} must require the market-data arm flag")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: assert_phase5d_aggr20_md.py <repo-root>")

    repo_root = Path(sys.argv[1]).resolve()
    script = repo_root / "scripts" / "vps" / "plaza2_aggr20_md_test_evidence.sh"
    run(["bash", "-n", str(script)], expect_success=True)
    refused = run(["bash", str(script)], expect_success=False)
    if "--bundle-root, --profile, --secret-env-file, and --output-dir are required" not in refused.stderr:
        raise AssertionError("AGGR20 evidence script did not fail closed without required arguments")

    validate_profile(repo_root / "profiles" / "test_plaza2_aggr20_md.template.yaml")
    validate_profile(repo_root / "profiles" / "test_plaza2_aggr20_md_local.example.yaml")

    print("phase5d AGGR20 profile and scripts validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
