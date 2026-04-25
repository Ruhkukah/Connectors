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
    section = profile.get("plaza2_trade_test_order_entry") or {}
    if not section:
        raise AssertionError(f"profile {path} is missing plaza2_trade_test_order_entry")
    if not profile.get("test_order_entry_armed_required") or not profile.get("test_tiny_order_armed_required"):
        raise AssertionError(f"profile {path} must require order-entry and tiny-order arms")
    if profile.get("environment") != "test":
        raise AssertionError(f"profile {path} must be TEST-only")
    enabled = section.get("live_enabled_commands") or []
    if enabled != ["AddOrder", "DelOrder"]:
        raise AssertionError(f"profile {path} must live-enable only AddOrder and DelOrder")
    disabled = set(section.get("live_disabled_commands") or [])
    expected_disabled = {
        "IcebergAddOrder",
        "IcebergDelOrder",
        "MoveOrder",
        "IcebergMoveOrder",
        "DelUserOrders",
        "DelOrdersByBFLimit",
        "CODHeartbeat",
    }
    if disabled != expected_disabled:
        raise AssertionError(f"profile {path} has unexpected disabled command list")
    runtime = section.get("runtime") or {}
    env_open = str(runtime.get("env_open_settings", ""))
    if "${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}" not in env_open:
        raise AssertionError(f"profile {path} must use the CGate software-key token")
    if "${MOEX_PLAZA2_TEST_CREDENTIALS}" in env_open:
        raise AssertionError(f"profile {path} must not use exchange credentials as CGate software key")
    order = section.get("tiny_order") or {}
    if int(order.get("quantity", 0)) > int(order.get("max_quantity", 0)):
        raise AssertionError(f"profile {path} tiny order exceeds max quantity")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: assert_phase5e_trade_order.py <repo-root>")

    repo_root = Path(sys.argv[1]).resolve()
    script = repo_root / "scripts" / "vps" / "plaza2_trade_test_order_evidence.sh"
    run(["bash", "-n", str(script)], expect_success=True)
    refused = run(["bash", str(script)], expect_success=False)
    if "--bundle-root, --profile, --secret-env-file, and --output-dir are required" not in refused.stderr:
        raise AssertionError("order-entry evidence script did not fail closed without required arguments")

    validate_profile(repo_root / "profiles" / "test_plaza2_trade_order_entry.template.yaml")
    validate_profile(repo_root / "profiles" / "test_plaza2_trade_order_entry_local.example.yaml")

    print("phase5e order-entry profile and scripts validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
