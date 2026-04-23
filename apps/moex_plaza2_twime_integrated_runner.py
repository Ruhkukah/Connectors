#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

from moex_phase0_common import load_json_yaml


def _find_runner() -> Path:
    candidates = [
        Path.cwd() / "apps" / "moex_plaza2_twime_integrated_test_runner",
        Path.cwd() / "build" / "apps" / "moex_plaza2_twime_integrated_test_runner",
        Path(__file__).resolve().parents[1] / "build" / "apps" / "moex_plaza2_twime_integrated_test_runner",
    ]
    runner = next((path for path in candidates if path.exists()), None)
    if runner is None:
        raise SystemExit(
            "missing built moex_plaza2_twime_integrated_test_runner executable for integrated TEST profile runs"
        )
    return runner


def _bool_arg(value: object) -> str:
    return "true" if bool(value) else "false"


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the Phase 4C integrated PLAZA II + TWIME TEST wrapper.")
    parser.add_argument("--profile", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--armed-test-network", action="store_true")
    parser.add_argument("--armed-test-session", action="store_true")
    parser.add_argument("--armed-test-plaza2", action="store_true")
    parser.add_argument("--armed-test-reconcile", action="store_true")
    parser.add_argument("--twime-credentials-env")
    parser.add_argument("--twime-credentials-file")
    parser.add_argument("--plaza-credentials-env")
    parser.add_argument("--plaza-credentials-file")
    parser.add_argument("--max-polls", type=int)
    parser.add_argument("--reconciler-stale-after-steps", type=int)
    args = parser.parse_args()

    profile_path = Path(args.profile).resolve()
    profile = load_json_yaml(profile_path)
    integrated = profile.get("plaza2_twime_integrated_test") or {}
    twime_tcp = profile.get("twime_tcp") or {}
    twime_live = profile.get("twime_live_session") or {}
    plaza = profile.get("plaza2_repl_test") or {}

    if not integrated:
        raise SystemExit("profile does not contain a plaza2_twime_integrated_test section")
    if not twime_tcp or not twime_live:
        raise SystemExit("profile must contain twime_tcp and twime_live_session sections")
    if not plaza:
        raise SystemExit("profile must contain a plaza2_repl_test section")

    twime_endpoint = twime_tcp.get("endpoint") or {}
    twime_gate = twime_tcp.get("test_network_gate") or {}
    twime_credentials = twime_tcp.get("credentials") or {}
    plaza_endpoint = plaza.get("endpoint") or {}
    plaza_runtime = plaza.get("runtime") or {}
    plaza_credentials = plaza.get("credentials") or {}
    plaza_session = plaza.get("session") or {}
    plaza_listeners = plaza.get("listeners") or {}

    command = [
        str(_find_runner()),
        "--profile-id",
        str(profile.get("profile_id", profile_path.stem)),
        "--output-dir",
        str(Path(args.output_dir).resolve()),
        "--twime-endpoint-host",
        str(twime_endpoint.get("host", "")),
        "--twime-endpoint-port",
        str(twime_endpoint.get("port", 0)),
        "--twime-allow-non-loopback",
        _bool_arg(twime_endpoint.get("allow_non_loopback", False)),
        "--twime-allow-non-localhost-dns",
        _bool_arg(twime_endpoint.get("allow_non_localhost_dns", False)),
        "--twime-external-test-endpoint-enabled",
        _bool_arg(twime_gate.get("external_test_endpoint_enabled", False)),
        "--twime-require-explicit-runtime-arm",
        _bool_arg(twime_gate.get("require_explicit_runtime_arm", True)),
        "--twime-block-production-like-hostnames",
        _bool_arg(twime_gate.get("block_production_like_hostnames", True)),
        "--twime-credentials-source",
        str(twime_credentials.get("source", "none")),
        "--twime-reconnect-enabled",
        _bool_arg(twime_live.get("reconnect_enabled", False)),
        "--twime-max-reconnect-attempts",
        str(twime_live.get("max_reconnect_attempts", 3)),
        "--twime-establish-deadline-ms",
        str(twime_live.get("establish_deadline_ms", 10000)),
        "--twime-graceful-terminate-timeout-ms",
        str(twime_live.get("graceful_terminate_timeout_ms", 3000)),
        "--plaza-endpoint-host",
        str(plaza_endpoint.get("host", "")),
        "--plaza-endpoint-port",
        str(plaza_endpoint.get("port", 0)),
        "--plaza-runtime-root",
        str(plaza_runtime.get("root", "")),
        "--plaza-env-open-settings",
        str(plaza_runtime.get("env_open_settings", "")),
        "--plaza-expected-spectra-release",
        str(plaza_runtime.get("expected_spectra_release", "")),
        "--plaza-connection-settings",
        str(plaza_session.get("connection_settings", "")),
        "--plaza-process-timeout-ms",
        str(plaza_session.get("process_timeout_ms", 50)),
        "--plaza-credentials-source",
        str(plaza_credentials.get("source", "none")),
        "--reconciler-stale-after-steps",
        str(
            args.reconciler_stale_after_steps
            if args.reconciler_stale_after_steps is not None
            else integrated.get("reconciler_stale_after_steps", 4)
        ),
        "--max-polls",
        str(args.max_polls if args.max_polls is not None else integrated.get("max_polls", 16)),
    ]

    twime_env = args.twime_credentials_env or str(twime_credentials.get("env_var", ""))
    twime_file = args.twime_credentials_file or str(
        twime_credentials.get("credentials_file", twime_credentials.get("file_path", ""))
    )
    if twime_env:
        command.extend(["--twime-credentials-env-var", twime_env])
    if twime_file:
        command.extend(["--twime-credentials-file", twime_file])

    library_path = str(plaza_runtime.get("library_path", ""))
    if library_path:
        command.extend(["--plaza-library-path", library_path])
    scheme_dir = str(plaza_runtime.get("scheme_dir", ""))
    if scheme_dir:
        command.extend(["--plaza-scheme-dir", scheme_dir])
    config_dir = str(plaza_runtime.get("config_dir", ""))
    if config_dir:
        command.extend(["--plaza-config-dir", config_dir])
    expected_hash = str(plaza_runtime.get("expected_scheme_sha256", ""))
    if expected_hash:
        command.extend(["--plaza-expected-scheme-sha256", expected_hash])
    connection_open_settings = str(plaza_session.get("connection_open_settings", ""))
    if connection_open_settings:
        command.extend(["--plaza-connection-open-settings", connection_open_settings])

    plaza_env = args.plaza_credentials_env or str(plaza_credentials.get("env_var", ""))
    plaza_file = args.plaza_credentials_file or str(
        plaza_credentials.get("credentials_file", plaza_credentials.get("file_path", ""))
    )
    if plaza_env:
        command.extend(["--plaza-credentials-env-var", plaza_env])
    if plaza_file:
        command.extend(["--plaza-credentials-file", plaza_file])

    for stream_name, stream_config in plaza_listeners.items():
        settings = str((stream_config or {}).get("settings", ""))
        command.extend(["--plaza-stream-settings", f"{stream_name}={settings}"])
        open_settings = str((stream_config or {}).get("open_settings", ""))
        if open_settings:
            command.extend(["--plaza-stream-open-settings", f"{stream_name}={open_settings}"])

    if args.armed_test_network:
        command.append("--armed-test-network")
    if args.armed_test_session:
        command.append("--armed-test-session")
    if args.armed_test_plaza2:
        command.append("--armed-test-plaza2")
    if args.armed_test_reconcile:
        command.append("--armed-test-reconcile")

    subprocess.run(command, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
