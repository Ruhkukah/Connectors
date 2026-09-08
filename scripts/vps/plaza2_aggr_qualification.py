#!/usr/bin/env python3
"""Private VPS launch adapter; never prints credentials or bypasses runner guards."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

import yaml


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--journal", type=Path, required=True)
    parser.add_argument("--isin", type=int, required=True)
    parser.add_argument("--session", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=36000)
    parser.add_argument("--orders", action="store_true")
    parser.add_argument("--idle", action="store_true")
    args = parser.parse_args()
    if args.orders and args.idle:
        parser.error("idle cannot enable orders")
    package = args.package.resolve()
    manifest = json.loads((package / "deployment.json").read_text())
    for name, expected in manifest["files"].items():
        path = Path(name)
        if not path.is_absolute():
            path = package / path
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise SystemExit("deployment hash mismatch; no process started")
    binary = package / "bin/plaza2_aggr_qualification"
    source_sha = subprocess.check_output([str(binary), "--version"], text=True).strip()
    if source_sha != manifest["source_sha"]:
        raise SystemExit("binary source identity mismatch")
    config = Path.home() / ".config/moex-connector"
    profile = yaml.safe_load((config / "profiles/plaza2_trade_test_order.local.yaml").read_text())
    account = profile["plaza2_trade_test_order_entry"]["tiny_order"]
    # Read the existing shell environment privately, as in previous TEST runs.
    raw = subprocess.check_output([
        "bash", "-c", 'set -a; source "$1"; env -0', "bash", str(config / "secrets/plaza2_test.env")
    ])
    env = dict(os.environ)
    for entry in raw.split(b"\0"):
        if b"=" in entry:
            key, value = entry.split(b"=", 1)
            if key in (b"MOEX_PLAZA2_TEST_CREDENTIALS", b"MOEX_PLAZA2_CGATE_SOFTWARE_KEY"):
                env[key.decode()] = value.decode()
    env["MOEX_AGGR_T1_AUTH"] = "20260910_AGGREGATED_QUALIFICATION"
    env["MOEX_AGGR_T1_JOURNAL"] = str(args.journal.resolve())
    env["MOEX_QUAL_BROKER"] = str(account["broker_code"])
    env["MOEX_QUAL_CLIENT"] = str(account["client_code"])
    env["MOEX_QUAL_ENV"] = (
        f"ini={config / 'cgate/client_t1.ini'};key=${{MOEX_PLAZA2_CGATE_SOFTWARE_KEY}}"
    )
    env.pop("MOEX_AGGR_T1_ORDER_AUTH", None)
    env.pop("MOEX_AGGR_T1_IDLE", None)
    if args.orders:
        env["MOEX_AGGR_T1_ORDER_AUTH"] = "20260910_ONE_LOT_ADD_CANCEL"
    if args.idle:
        env["MOEX_AGGR_T1_IDLE"] = "1"
    runtime = Path(manifest["runtime_root"])
    command = [
        str(binary), str(args.output.resolve()), str(args.seconds), "plaza2", "qualify",
        "--runtime-root", str(runtime), "--library-path", manifest["runtime_library"],
        "--scheme-dir", manifest["scheme_dir"], "--config-dir", manifest["config_dir"],
        "--env-settings-var", "MOEX_QUAL_ENV", "--broker-code-env", "MOEX_QUAL_BROKER",
        "--client-code-env", "MOEX_QUAL_CLIENT", "--isin-id", str(args.isin),
        "--session-id", str(args.session), "--expected-release", "SPECTRA9.9.0",
        "--armed-test-network", "--armed-test-session", "--armed-test-plaza2", "--json",
    ]
    work = args.output.parent / (args.output.name + "-vendor")
    work.mkdir(mode=0o700)
    os.umask(0o077)
    os.chdir(work)
    os.execve(str(binary), command, env)


if __name__ == "__main__":
    main()
