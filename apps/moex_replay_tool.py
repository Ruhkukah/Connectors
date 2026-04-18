#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from moex_phase0_common import load_json_yaml


def main() -> int:
    parser = argparse.ArgumentParser(description="Phase 0 replay tool stub.")
    parser.add_argument("--profile", required=True)
    args = parser.parse_args()

    profile = load_json_yaml(Path(args.profile).resolve())
    print(f"replay tool stub loaded profile={profile['profile_id']} environment={profile['environment']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
