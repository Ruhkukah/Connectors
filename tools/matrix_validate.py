#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from moex_phase0_common import load_json_yaml


def load_ids(items: list[dict], id_key: str) -> set[str]:
    identifiers = [item[id_key] for item in items]
    duplicates = {identifier for identifier in identifiers if identifiers.count(identifier) > 1}
    if duplicates:
        raise ValueError(f"Duplicate IDs for {id_key}: {sorted(duplicates)}")
    return set(identifiers)


def ensure_refs(name: str, source: list[dict], key: str, allowed: set[str]) -> None:
    missing = sorted({item[key] for item in source if item.get(key) and item[key] not in allowed})
    if missing:
        raise ValueError(f"{name} has missing references in {key}: {missing}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate Phase 0 matrix files and referential integrity.")
    parser.add_argument("--matrix-dir", required=True)
    args = parser.parse_args()

    matrix_dir = Path(args.matrix_dir).resolve()
    artifacts = load_json_yaml(matrix_dir / "artifacts.yaml")["artifacts"]
    scenarios = load_json_yaml(matrix_dir / "cert_scenarios.yaml")["scenarios"]
    implementation = load_json_yaml(matrix_dir / "implementation_coverage.yaml")["coverage"]
    osengine_inventory = load_json_yaml(matrix_dir / "osengine_inventory.yaml")["inventory"]
    adapter_map = load_json_yaml(matrix_dir / "alorengine_adapter_map.yaml")["adapter_map"]
    event_map = load_json_yaml(matrix_dir / "alorengine_event_map.yaml")["event_map"]
    gaps = load_json_yaml(matrix_dir / "gaps.yaml")["gaps"]

    protocol_items = []
    protocol_dir = matrix_dir / "protocol_inventory"
    for file_path in sorted(protocol_dir.glob("*.yaml")):
        protocol_items.extend(load_json_yaml(file_path)["items"])

    artifact_ids = load_ids(artifacts, "artifact_id")
    scenario_ids = load_ids(scenarios, "scenario_id")
    protocol_item_ids = load_ids(protocol_items, "protocol_item_id")
    coverage_ids = load_ids(implementation, "coverage_id")
    inventory_ids = load_ids(osengine_inventory, "inventory_id")
    adapter_ids = load_ids(adapter_map, "adapter_map_id")
    native_event_ids = load_ids(event_map, "native_event_id")
    gap_ids = load_ids(gaps, "gap_id")

    ensure_refs("protocol_inventory", protocol_items, "artifact_id", artifact_ids)
    ensure_refs("cert_scenarios", scenarios, "artifact_id", artifact_ids)
    ensure_refs("implementation_coverage", implementation, "protocol_item_id", protocol_item_ids)
    ensure_refs("implementation_coverage", implementation, "scenario_id", scenario_ids)
    ensure_refs("implementation_coverage", implementation, "inventory_id", inventory_ids)
    ensure_refs("alorengine_adapter_map", adapter_map, "coverage_id", coverage_ids)
    ensure_refs("alorengine_event_map", event_map, "adapter_map_id", adapter_ids)
    ensure_refs("gaps", gaps, "artifact_id", artifact_ids)
    ensure_refs("gaps", gaps, "protocol_item_id", protocol_item_ids)
    ensure_refs("gaps", gaps, "coverage_id", coverage_ids)

    print("matrix validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
