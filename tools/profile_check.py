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


def _contains_inline_credentials(credentials: dict) -> bool:
    return any(key in credentials for key in ("value", "credentials", "secret", "password"))


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
                raise SystemExit("twime_tcp profiles are Phase 2F test-only and must use environment=test")

            endpoint = twime_tcp.get("endpoint") or {}
            host = str(endpoint.get("host", "127.0.0.1"))
            if _looks_banned_host(host):
                raise SystemExit("twime_tcp endpoint host looks like a live MOEX/broker target and is blocked in Phase 2F")

            test_network_gate = twime_tcp.get("test_network_gate") or {}
            external_enabled = bool(test_network_gate.get("external_test_endpoint_enabled", False))
            credentials = twime_tcp.get("credentials") or {}
            if _contains_inline_credentials(credentials):
                raise SystemExit("tracked twime_tcp profiles must not contain inline credentials")

            if _looks_non_loopback(host):
                if _looks_placeholder(host):
                    if not external_enabled:
                        raise SystemExit(
                            "twime_tcp placeholder host requires external_test_endpoint_enabled=true in Phase 2F"
                        )
                else:
                    raise SystemExit(
                        "tracked twime_tcp profiles must not contain real non-loopback hosts in Phase 2F; "
                        "use an untracked local override plus --armed-test-network"
                    )

        twime_live_session = profile.get("twime_live_session")
        if isinstance(twime_live_session, dict) and environment != "test":
            raise SystemExit("twime_live_session profiles are Phase 2F test-only and must use environment=test")

        plaza2_repl_test = profile.get("plaza2_repl_test")
        if isinstance(plaza2_repl_test, dict):
            if environment != "test":
                raise SystemExit("plaza2_repl_test profiles are Phase 3F test-only and must use environment=test")

            endpoint = plaza2_repl_test.get("endpoint") or {}
            host = str(endpoint.get("host", ""))
            if _looks_banned_host(host):
                raise SystemExit("plaza2_repl_test endpoint host looks like a live MOEX/broker target and is blocked")

            credentials = plaza2_repl_test.get("credentials") or {}
            if _contains_inline_credentials(credentials):
                raise SystemExit("tracked plaza2_repl_test profiles must not contain inline credentials")

            if _looks_non_loopback(host) and not _looks_placeholder(host):
                raise SystemExit(
                    "tracked plaza2_repl_test profiles must not contain real non-loopback hosts in Phase 3F; "
                    "use an untracked local override plus all required --armed-test-* flags"
                )

        plaza2_aggr20_md_test = profile.get("plaza2_aggr20_md_test")
        if isinstance(plaza2_aggr20_md_test, dict):
            if environment != "test":
                raise SystemExit("plaza2_aggr20_md_test profiles are Phase 5D test-only and must use environment=test")

            endpoint = plaza2_aggr20_md_test.get("endpoint") or {}
            host = str(endpoint.get("host", ""))
            if _looks_banned_host(host):
                raise SystemExit("plaza2_aggr20_md_test endpoint host looks like a live MOEX/broker target and is blocked")

            credentials = plaza2_aggr20_md_test.get("credentials") or {}
            if _contains_inline_credentials(credentials):
                raise SystemExit("tracked plaza2_aggr20_md_test profiles must not contain inline credentials")

            if _looks_non_loopback(host) and not _looks_placeholder(host):
                raise SystemExit(
                    "tracked plaza2_aggr20_md_test profiles must not contain real non-loopback hosts in Phase 5D; "
                    "use an untracked local override plus all required --armed-test-* flags"
                )

            stream = plaza2_aggr20_md_test.get("stream") or {}
            stream_name = str(stream.get("name", ""))
            stream_settings = str(stream.get("settings", ""))
            if stream_name != "FORTS_AGGR20_REPL" or "FORTS_AGGR20_REPL" not in stream_settings:
                raise SystemExit("Phase 5D tracked public market-data profiles may enable only FORTS_AGGR20_REPL")
            for forbidden in ("FORTS_ORDLOG_REPL", "FORTS_ORDBOOK_REPL", "FORTS_DEALS_REPL"):
                if forbidden in stream_settings:
                    raise SystemExit(f"Phase 5D tracked profiles must not enable {forbidden}")

        plaza2_twime_integrated_test = profile.get("plaza2_twime_integrated_test")
        if isinstance(plaza2_twime_integrated_test, dict):
            if environment != "test":
                raise SystemExit(
                    "plaza2_twime_integrated_test profiles are Phase 4C test-only and must use environment=test"
                )

            if not isinstance(twime_live_session, dict) or not isinstance(twime_tcp, dict):
                raise SystemExit(
                    "plaza2_twime_integrated_test profiles must include twime_tcp and twime_live_session sections"
                )

            if not isinstance(plaza2_repl_test, dict):
                raise SystemExit(
                    "plaza2_twime_integrated_test profiles must include a plaza2_repl_test section"
                )

        print(f"profile {profile['profile_id']} validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
