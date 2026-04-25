#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

from moex_phase0_common import load_json_yaml


def _find_runner() -> Path:
    candidates = [
        Path.cwd() / "apps" / "moex_plaza2_trade_test_order_entry_runner",
        Path.cwd() / "build-docker-linux" / "apps" / "moex_plaza2_trade_test_order_entry_runner",
        Path.cwd() / "build" / "apps" / "moex_plaza2_trade_test_order_entry_runner",
        Path(__file__).resolve().parents[1] / "build-docker-linux" / "apps" / "moex_plaza2_trade_test_order_entry_runner",
        Path(__file__).resolve().parents[1] / "build" / "apps" / "moex_plaza2_trade_test_order_entry_runner",
    ]
    runner = next((path for path in candidates if path.exists()), None)
    if runner is None:
        raise SystemExit("missing built moex_plaza2_trade_test_order_entry_runner executable")
    return runner


def _append_if(command: list[str], option: str, value: object) -> None:
    text = "" if value is None else str(value)
    if text:
        command.extend([option, text])


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Phase 5E PLAZA II TEST order-entry wrapper.")
    parser.add_argument("--profile", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--armed-test-network", action="store_true")
    parser.add_argument("--armed-test-session", action="store_true")
    parser.add_argument("--armed-test-plaza2", action="store_true")
    parser.add_argument("--armed-test-order-entry", action="store_true")
    parser.add_argument("--armed-test-tiny-order", action="store_true")
    parser.add_argument("--send-test-order", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--max-polls", type=int, default=512)
    args = parser.parse_args()

    profile_path = Path(args.profile).resolve()
    profile = load_json_yaml(profile_path)
    section = profile.get("plaza2_trade_test_order_entry") or {}
    if not section:
        raise SystemExit("profile does not contain a plaza2_trade_test_order_entry section")

    runtime = section.get("runtime") or {}
    session = section.get("session") or {}
    endpoint = section.get("endpoint") or {}
    streams = section.get("private_streams") or {}
    credentials = section.get("credentials") or {}
    software_key = section.get("software_key") or {}
    publisher = section.get("publisher") or {}
    order = section.get("tiny_order") or {}

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
        "--publisher-settings",
        str(publisher.get("settings", "")),
        "--credentials-source",
        str(credentials.get("source", "none")),
        "--software-key-source",
        str(software_key.get("source", "none")),
        "--isin-id",
        str(order.get("isin_id", 0)),
        "--broker-code",
        str(order.get("broker_code", "")),
        "--client-code",
        str(order.get("client_code", "")),
        "--side",
        str(order.get("side", "")),
        "--price",
        str(order.get("price", "")),
        "--quantity",
        str(order.get("quantity", 0)),
        "--max-quantity",
        str(order.get("max_quantity", 1)),
        "--ext-id",
        str(order.get("ext_id", 0)),
        "--client-transaction-id-prefix",
        str(order.get("client_transaction_id_prefix", "")),
        "--max-polls",
        str(args.max_polls),
    ]

    for option, key in (
        ("--library-path", "library_path"),
        ("--scheme-dir", "scheme_dir"),
        ("--config-dir", "config_dir"),
        ("--expected-spectra-release", "expected_spectra_release"),
        ("--expected-scheme-sha256", "expected_scheme_sha256"),
    ):
        _append_if(command, option, runtime.get(key, ""))

    _append_if(command, "--publisher-open-settings", publisher.get("open_settings", ""))
    _append_if(command, "--comment", order.get("comment", ""))
    _append_if(command, "--credentials-env-var", credentials.get("env_var", ""))
    _append_if(command, "--software-key-env-var", software_key.get("env_var", ""))

    stream_options = (
        ("--trade-stream-settings", "FORTS_TRADE_REPL"),
        ("--userorderbook-stream-settings", "FORTS_USERORDERBOOK_REPL"),
        ("--pos-stream-settings", "FORTS_POS_REPL"),
        ("--part-stream-settings", "FORTS_PART_REPL"),
        ("--refdata-stream-settings", "FORTS_REFDATA_REPL"),
    )
    for option, name in stream_options:
        _append_if(command, option, (streams.get(name) or {}).get("settings", ""))
    _append_if(command, "--stream-open-settings", section.get("stream_open_settings", ""))

    if args.armed_test_network:
        command.append("--armed-test-network")
    if args.armed_test_session:
        command.append("--armed-test-session")
    if args.armed_test_plaza2:
        command.append("--armed-test-plaza2")
    if args.armed_test_order_entry:
        command.append("--armed-test-order-entry")
    if args.armed_test_tiny_order:
        command.append("--armed-test-tiny-order")
    if args.send_test_order:
        command.append("--send-test-order")
    else:
        command.append("--dry-run")

    subprocess.run(command, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
