#!/usr/bin/env python3
"""Private VPS launch adapter; never prints credentials or bypasses runner guards."""
import argparse
import hashlib
import json
import os
import re
import time
from datetime import datetime, timezone, timedelta
from pathlib import Path
import subprocess

import yaml


def main(authorization_date="2026-09-10", binary_name="plaza2_aggr_qualification", token=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--journal", type=Path, required=True)
    parser.add_argument("--isin", type=int, required=True)
    parser.add_argument("--session", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=36000)
    parser.add_argument("--symbol", required=True)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.journal = args.journal.resolve()
    if "MOEX_AGGR_T1_ORDER_AUTH" in os.environ or "MOEX_AGGR_T1_IDLE" in os.environ:
        parser.error("observation rejects order authorization and idle mode")
    now = datetime.now(timezone(timedelta(hours=3)))
    if now.date().isoformat() != authorization_date or not 418 <= now.hour * 60 + now.minute < 970:
        parser.error(f"outside {authorization_date} 06:58-16:10 MSK observation window")
    if args.output.exists():
        parser.error("output must be new; existing scenario or order.request is invalid preparation")
    if args.journal.exists() and any(args.journal.iterdir()):
        parser.error("journal must be empty")
    os.umask(0o077)
    package = args.package.resolve()
    manifest = json.loads((package / "deployment.json").read_text())
    for name, expected in manifest["files"].items():
        path = Path(name)
        if not path.is_absolute():
            path = package / path
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise SystemExit("deployment hash mismatch; no process started")
    binary = package / "bin" / binary_name
    if authorization_date != "2026-09-10":
        if manifest.get("authorization_date") != authorization_date:
            raise SystemExit("deployment authorization date mismatch")
        if hashlib.sha256(binary.read_bytes()).hexdigest() != manifest.get("binary_sha256"):
            raise SystemExit("binary hash mismatch")
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
    env["MOEX_AGGR_T1_AUTH"] = token or authorization_date.replace("-", "") + "_AGGREGATED_OBSERVATION"
    env["MOEX_AGGR_FORENSIC_SYMBOL"] = args.symbol
    env["MOEX_AGGR_T1_JOURNAL"] = str(args.journal.resolve())
    env["MOEX_QUAL_BROKER"] = str(account["broker_code"])
    env["MOEX_QUAL_CLIENT"] = str(account["client_code"])
    work = args.output.parent / (args.output.name + "-vendor")
    work.mkdir(mode=0o700)
    private_ini = work / "client.ini"
    client_settings, changed = re.subn(
        r"(?m)^logfile=.*$", f"logfile={work / 'vendor.log'}",
        (config / "cgate/client_t1.ini").read_text(),
    )
    if changed != 1:
        raise SystemExit("expected one existing client logfile setting")
    private_ini.write_text(client_settings)
    (work / "launch.json").write_text(json.dumps({
        "source_sha": source_sha, "created_utc_s": time.time(),
        "manifest_sha256": hashlib.sha256((package / "deployment.json").read_bytes()).hexdigest(),
        "client_ini_sha256": hashlib.sha256(private_ini.read_bytes()).hexdigest(),
        "orders_enabled": False, "symbol": args.symbol, "isin_id": args.isin, "session_id": args.session,
    }, indent=2) + "\n")
    env["MOEX_QUAL_ENV"] = f"ini={private_ini};key=${{MOEX_PLAZA2_CGATE_SOFTWARE_KEY}}"
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
    os.chdir(work)
    os.execve(str(binary), command, env)


if __name__ == "__main__":
    main()
