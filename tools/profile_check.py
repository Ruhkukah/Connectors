#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from moex_phase0_common import load_json_yaml


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate Phase 0 profile templates and production arming rules.")
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

        print(f"profile {profile['profile_id']} validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
