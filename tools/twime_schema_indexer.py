#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from moex_phase0_common import dump_yaml, ensure_dir
from twime_schema_common import parse_twime_schema


def main() -> int:
    parser = argparse.ArgumentParser(description="Index TWIME schema XML into machine-readable inventory files.")
    parser.add_argument("--schema", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    schema_path = Path(args.schema).resolve()
    out_dir = ensure_dir(Path(args.out).resolve())
    parsed = parse_twime_schema(schema_path)

    dump_yaml({"version": 1, "items": parsed["messages"]}, out_dir / "twime_messages.yaml")
    dump_yaml({"version": 1, "items": parsed["types"]}, out_dir / "twime_types.yaml")
    dump_yaml({"version": 1, "items": parsed["enums"]}, out_dir / "twime_enums.yaml")
    dump_yaml({"version": 1, "items": parsed["fields"]}, out_dir / "twime_fields.yaml")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
