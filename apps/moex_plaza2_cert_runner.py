#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

from moex_phase0_common import load_json_yaml


def _find_runner() -> Path:
    candidates = [
        Path.cwd() / "apps" / "moex_plaza2_test_runner",
        Path.cwd() / "build-docker-linux" / "apps" / "moex_plaza2_test_runner",
        Path.cwd() / "build" / "apps" / "moex_plaza2_test_runner",
        Path(__file__).resolve().parents[1] / "build-docker-linux" / "apps" / "moex_plaza2_test_runner",
        Path(__file__).resolve().parents[1] / "build" / "apps" / "moex_plaza2_test_runner",
    ]
    runner = next((path for path in candidates if path.exists()), None)
    if runner is None:
        raise SystemExit("missing built moex_plaza2_test_runner executable for PLAZA II TEST profile runs")
    return runner


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the Phase 3F PLAZA II TEST bring-up wrapper.")
    parser.add_argument("--profile", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--armed-test-network", action="store_true")
    parser.add_argument("--armed-test-session", action="store_true")
    parser.add_argument("--armed-test-plaza2", action="store_true")
    parser.add_argument("--credentials-env")
    parser.add_argument("--credentials-file")
    parser.add_argument("--max-polls", type=int, default=8)
    args = parser.parse_args()

    profile_path = Path(args.profile).resolve()
    profile = load_json_yaml(profile_path)
    plaza2_test = profile.get("plaza2_repl_test") or {}
    if not plaza2_test:
        raise SystemExit("profile does not contain a plaza2_repl_test section")

    runtime = plaza2_test.get("runtime") or {}
    session = plaza2_test.get("session") or {}
    credentials = plaza2_test.get("credentials") or {}
    listeners = plaza2_test.get("listeners") or {}
    endpoint = plaza2_test.get("endpoint") or {}

    command = [
        str(_find_runner()),
        "--profile-id",
        str(profile.get("profile_id", profile_path.stem)),
        "--output-dir",
        str(Path(args.output_dir).resolve()),
        "--endpoint-host",
        str(endpoint.get("host", "")),
        "--endpoint-port",
        str(endpoint.get("port", 0)),
        "--runtime-root",
        str(runtime.get("root", "")),
        "--env-open-settings",
        str(runtime.get("env_open_settings", "")),
        "--connection-settings",
        str(session.get("connection_settings", "")),
        "--process-timeout-ms",
        str(session.get("process_timeout_ms", 50)),
        "--credentials-source",
        str(credentials.get("source", "none")),
        "--max-polls",
        str(args.max_polls),
    ]

    library_path = str(runtime.get("library_path", ""))
    if library_path:
        command.extend(["--library-path", library_path])
    scheme_dir = str(runtime.get("scheme_dir", ""))
    if scheme_dir:
        command.extend(["--scheme-dir", scheme_dir])
    config_dir = str(runtime.get("config_dir", ""))
    if config_dir:
        command.extend(["--config-dir", config_dir])
    expected_release = str(runtime.get("expected_spectra_release", ""))
    if expected_release:
        command.extend(["--expected-spectra-release", expected_release])
    expected_hash = str(runtime.get("expected_scheme_sha256", ""))
    if expected_hash:
        command.extend(["--expected-scheme-sha256", expected_hash])
    connection_open_settings = str(session.get("connection_open_settings", ""))
    if connection_open_settings:
        command.extend(["--connection-open-settings", connection_open_settings])

    env_var = args.credentials_env or str(credentials.get("env_var", ""))
    file_path = args.credentials_file or str(credentials.get("credentials_file", credentials.get("file_path", "")))
    if env_var:
        command.extend(["--credentials-env-var", env_var])
    if file_path:
        command.extend(["--credentials-file", file_path])

    for stream_name, stream_config in listeners.items():
        stream_settings = str((stream_config or {}).get("settings", ""))
        command.extend(["--stream-settings", f"{stream_name}={stream_settings}"])
        stream_open_settings = str((stream_config or {}).get("open_settings", ""))
        if stream_open_settings:
            command.extend(["--stream-open-settings", f"{stream_name}={stream_open_settings}"])

    if args.armed_test_network:
        command.append("--armed-test-network")
    if args.armed_test_session:
        command.append("--armed-test-session")
    if args.armed_test_plaza2:
        command.append("--armed-test-plaza2")

    subprocess.run(command, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
