#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from moex_phase0_common import dump_json, ensure_dir, load_json_yaml, redact_secrets


def main() -> int:
    parser = argparse.ArgumentParser(description="Load a stub certification scenario and emit deterministic stub logs.")
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    scenario_path = Path(args.scenario).resolve()
    output_dir = ensure_dir(Path(args.output_dir).resolve())
    scenario = load_json_yaml(scenario_path)
    scenario_id = scenario["scenario_id"]

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
