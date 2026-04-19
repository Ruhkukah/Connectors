#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

from moex_phase0_common import dump_json, ensure_dir, load_json_yaml, redact_secrets


def main() -> int:
    parser = argparse.ArgumentParser(description="Load a stub certification scenario and emit deterministic stub logs.")
    parser.add_argument("--scenario")
    parser.add_argument("--profile")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--armed-test-network", action="store_true")
    parser.add_argument("--armed-test-session", action="store_true")
    parser.add_argument("--credentials-env")
    parser.add_argument("--credentials-file")
    parser.add_argument("--connect-timeout-ms", type=int)
    parser.add_argument("--terminate-timeout-ms", type=int)
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()

    output_dir = ensure_dir(Path(args.output_dir).resolve())

    if not args.scenario and not args.profile:
        raise SystemExit("provide --scenario and/or --profile")

    if args.profile:
        profile_path = Path(args.profile).resolve()
        profile = load_json_yaml(profile_path)
        twime_tcp = profile.get("twime_tcp") or {}
        endpoint = twime_tcp.get("endpoint") or {}
        gate = twime_tcp.get("test_network_gate") or {}
        credentials = twime_tcp.get("credentials") or {}
        live_session = profile.get("twime_live_session") or {}

        candidates = [
            Path.cwd() / "apps" / "moex_twime_cert_runner",
            Path.cwd() / "build" / "apps" / "moex_twime_cert_runner",
            Path(__file__).resolve().parents[1] / "build" / "apps" / "moex_twime_cert_runner",
        ]
        runner = next((path for path in candidates if path.exists()), None)
        if runner is None:
            raise SystemExit("missing built moex_twime_cert_runner executable for TWIME TCP profile runs")

        credentials_source = str(credentials.get("source", "none"))
        env_var = args.credentials_env or str(credentials.get("env_var", ""))
        file_path = args.credentials_file or str(credentials.get("credentials_file", credentials.get("file_path", "")))
        scenario_id = None
        if args.scenario:
            scenario = load_json_yaml(Path(args.scenario).resolve())
            scenario_id = scenario["scenario_id"]

        command = [
            str(runner),
            "--profile-id",
            str(profile.get("profile_id", profile_path.stem)),
            "--output-dir",
            str(output_dir),
            "--endpoint-host",
            str(endpoint.get("host", "127.0.0.1")),
            "--endpoint-port",
            str(endpoint.get("port", 0)),
            "--external-test-endpoint-enabled",
            "1" if bool(gate.get("external_test_endpoint_enabled", False)) else "0",
            "--require-explicit-runtime-arm",
            "1" if bool(gate.get("require_explicit_runtime_arm", True)) else "0",
            "--block-production-like-hostnames",
            "1" if bool(gate.get("block_production_like_hostnames", True)) else "0",
            "--block-private-nonlocal-networks",
            "1" if bool(gate.get("block_private_nonlocal_networks_by_default", False)) else "0",
            "--credentials-source",
            credentials_source,
            "--reconnect-enabled",
            "1" if bool(live_session.get("reconnect_enabled", False)) else "0",
            "--max-reconnect-attempts",
            str(live_session.get("max_reconnect_attempts", 3)),
            "--establish-deadline-ms",
            str(live_session.get("establish_deadline_ms", 10000)),
            "--graceful-terminate-timeout-ms",
            str(live_session.get("graceful_terminate_timeout_ms", 3000)),
        ]

        if scenario_id:
            command.extend(["--scenario-id", scenario_id])
        if args.armed_test_network:
            command.append("--armed-test-network")
        if args.armed_test_session:
            command.append("--armed-test-session")
        if args.validate_only:
            command.append("--validate-only")
        if env_var:
            command.extend(["--credentials-env-var", env_var])
        if file_path:
            command.extend(["--credentials-file", file_path])
        if args.connect_timeout_ms is not None:
            command.extend(["--connect-timeout-ms", str(args.connect_timeout_ms)])
        if args.terminate_timeout_ms is not None:
            command.extend(["--terminate-timeout-ms", str(args.terminate_timeout_ms)])

        subprocess.run(command, check=True)
        return 0

    scenario_path = Path(args.scenario).resolve()
    scenario = load_json_yaml(scenario_path)
    scenario_id = scenario["scenario_id"]

    if scenario_id.startswith("twime_live_test_session_"):
        raise SystemExit("live TWIME test-session scenarios require --profile")

    if scenario_id.startswith("twime_"):
        candidates = [
            Path.cwd() / "apps" / "moex_twime_cert_runner",
            Path.cwd() / "build" / "apps" / "moex_twime_cert_runner",
            Path(__file__).resolve().parents[1] / "build" / "apps" / "moex_twime_cert_runner",
        ]
        runner = next((path for path in candidates if path.exists()), None)
        if runner is None:
            raise SystemExit("missing built moex_twime_cert_runner executable for synthetic TWIME scenarios")
        subprocess.run(
            [str(runner), "--scenario-id", scenario_id, "--output-dir", str(output_dir)],
            check=True,
        )
        return 0

    lines = [f"scenario={scenario_id}", f"title={scenario['title']}"]
    for step in scenario["steps"]:
        lines.append(f"step={step['step_id']} action={step['action']} expected={step['expected']}")

    log_path = output_dir / f"{scenario_id}.cert.log"
    log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    summary = {
        "scenario_id": scenario_id,
        "step_count": len(scenario["steps"]),
        "redacted_metadata": redact_secrets(scenario.get("metadata", {})),
        "log_path": str(log_path),
    }
    dump_json(summary, output_dir / f"{scenario_id}.summary.json")
    print(f"stub certification log emitted: {log_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
