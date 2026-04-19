#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from moex_phase0_common import load_json_yaml


def _looks_non_loopback(host: str) -> bool:
    normalized = host.strip().lower()
    return normalized not in {"127.0.0.1", "::1", "localhost"}


def _looks_placeholder(host: str) -> bool:
    return "placeholder" in host.strip().lower()


def _looks_banned_host(host: str) -> bool:
    normalized = host.strip().lower()
    return any(token in normalized for token in ("moex", "spectra", "alor", "broker"))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate checked-in profile templates, production arming rules, and Phase 2E TWIME test-endpoint safety."
    )
    parser.add_argument("--profile", required=True, nargs="+")
    parser.add_argument("--armed", action="store_true")
    args = parser.parse_args()

    for profile_arg in args.profile:
        profile = load_json_yaml(Path(profile_arg).resolve())
        required = ["profile_id", "environment", "broker_topology", "shadow_mode", "connectors"]
        missing = [key for key in required if key not in profile]
        if missing:
            raise SystemExit(f"Profile missing required keys: {missing}")

        environment = str(profile["environment"]).lower()
        if environment == "prod" and not args.armed:
            raise SystemExit("prod profiles require explicit --armed")

        twime_tcp = profile.get("twime_tcp")
        if isinstance(twime_tcp, dict):
            if environment != "test":
                raise SystemExit("twime_tcp profiles are Phase 2D test-only and must use environment=test")

            endpoint = twime_tcp.get("endpoint") or {}
            host = str(endpoint.get("host", "127.0.0.1"))
            if _looks_banned_host(host):
                raise SystemExit("twime_tcp endpoint host looks like a live MOEX/broker target and is blocked in Phase 2E")

            test_network_gate = twime_tcp.get("test_network_gate") or {}
            external_enabled = bool(test_network_gate.get("external_test_endpoint_enabled", False))

            if _looks_non_loopback(host):
                if _looks_placeholder(host):
                    if not external_enabled:
                        raise SystemExit(
                            "twime_tcp placeholder host requires external_test_endpoint_enabled=true in Phase 2E"
                        )
                else:
                    raise SystemExit(
                        "tracked twime_tcp profiles must not contain real non-loopback hosts in Phase 2E; "
                        "use an untracked local override plus --armed-test-network"
                    )

        print(f"profile {profile['profile_id']} validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
