#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path


SCRIPT_NAMES = [
    "package_plaza2_test_bundle.sh",
    "fetch_cgate_distributive.sh",
    "install_cgate_linux.sh",
    "plaza2_test_preflight.sh",
    "plaza2_repl_test_evidence.sh",
    "plaza2_change_test_password.sh",
]


EXPECTED_PLACEHOLDERS = [
    "MOEX_PLAZA2_TEST_CREDENTIALS='<operator-local-value>'",
    "CGATE_DISTRIBUTIVE_URL='<official MOEX CGate Linux URL>'",
]


def run(command: list[str], *, expect_success: bool) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if expect_success and result.returncode != 0:
        raise AssertionError(f"command failed unexpectedly: {command}\n{result.stderr}")
    if not expect_success and result.returncode == 0:
        raise AssertionError(f"command succeeded unexpectedly: {command}\n{result.stdout}")
    return result


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: assert_phase4d_vps_scripts.py <repo-root>")

    repo_root = Path(sys.argv[1]).resolve()
    script_dir = repo_root / "scripts" / "vps"

    for name in SCRIPT_NAMES:
        script = script_dir / name
        if not script.exists():
            raise AssertionError(f"missing script: {script}")
        run(["bash", "-n", str(script)], expect_success=True)
        run(["bash", str(script)], expect_success=False)

    fetch = script_dir / "fetch_cgate_distributive.sh"
    rejected = run(
        ["bash", str(fetch), "--url", "https://example.invalid/cgate.zip", "--out-dir", "/tmp/moex-phase4d-test"],
        expect_success=False,
    )
    if "refusing non-MOEX URL" not in rejected.stderr:
        raise AssertionError("fetch script did not fail closed on non-MOEX URL")

    doc = repo_root / "docs" / "plaza2_phase4d_vps_cgate_install_evidence.md"
    text = doc.read_text(encoding="utf-8")
    for placeholder in EXPECTED_PLACEHOLDERS:
        if placeholder not in text:
            raise AssertionError(f"expected placeholder missing from docs: {placeholder}")

    print("phase4d VPS scripts validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
