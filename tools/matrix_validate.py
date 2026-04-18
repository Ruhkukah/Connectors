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


def ensure_fields(name: str, source: list[dict], fields: list[str]) -> None:
    missing = []
    for item in source:
        for field in fields:
            value = item.get(field)
            if value is None or (isinstance(value, str) and not value.strip()):
                missing.append((item, field))
    if missing:
        rendered = [f"{entry.get('gap_id', entry.get('scenario_id', '<unknown>'))}:{field}" for entry, field in missing]
        raise ValueError(f"{name} has missing required fields: {rendered}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate Phase 0 matrix files and referential integrity.")
    parser.add_argument("--matrix-dir", required=True)
    args = parser.parse_args()

    matrix_dir = Path(args.matrix_dir).resolve()
    project_root = matrix_dir.parent
    artifacts = load_json_yaml(matrix_dir / "artifacts.yaml")["artifacts"]
    scenarios = load_json_yaml(matrix_dir / "cert_scenarios.yaml")["scenarios"]
    connectors = load_json_yaml(matrix_dir / "connectors.yaml")["connectors"]
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
    connector_ids = load_ids(connectors, "connector_id")
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
    ensure_fields("gaps", gaps, ["gap_id", "owner", "status", "next_action"])

    coverage_by_scenario = {scenario_id: 0 for scenario_id in scenario_ids}
    for row in implementation:
        coverage_by_scenario[row["scenario_id"]] += 1
    uncovered_scenarios = sorted([scenario_id for scenario_id, count in coverage_by_scenario.items() if count == 0])
    if uncovered_scenarios:
        raise ValueError(f"cert scenarios without coverage rows: {uncovered_scenarios}")

    profiles_dir = project_root / "profiles"
    profiles = [load_json_yaml(path) for path in sorted(profiles_dir.glob("*.yaml"))]
    profile_connectors: set[str] = set()
    test_replay_connectors: set[str] = set()
    for profile in profiles:
        for connector_group in profile["connectors"].values():
            for connector_id in connector_group:
                if connector_id not in connector_ids:
                    raise ValueError(f"profile {profile['profile_id']} references unknown connector {connector_id}")
                profile_connectors.add(connector_id)
                if str(profile["environment"]).lower() in {"test", "replay"}:
                    test_replay_connectors.add(connector_id)

    required_profile_coverage = {
        connector["connector_id"]
        for connector in connectors
        if connector.get("requires_test_or_replay_coverage", True)
    }
    missing_test_replay = sorted(required_profile_coverage - test_replay_connectors)
    if missing_test_replay:
        raise ValueError(f"connectors without test/replay profile coverage: {missing_test_replay}")

    lock_config = load_json_yaml(project_root / "spec-lock" / "lock_roots.json")
    for root_spec in lock_config["roots"]:
        required_artifacts = root_spec.get("required_artifacts", [])
        if not required_artifacts:
            continue
        manifest_path = project_root / root_spec["manifest"]
        if not manifest_path.exists():
            unresolved = [
                item["relative_path"] if isinstance(item, dict) else item
                for item in required_artifacts
                if not (isinstance(item, dict) and item.get("expectation") == "needs_confirmation")
            ]
            if unresolved:
                raise ValueError(f"missing manifest for required artifacts in root {root_spec['artifact_id']}: {manifest_path}")
            continue

        manifest = load_json_yaml(manifest_path)
        rows_by_path = {row["relative_path"]: row for row in manifest["artifacts"]}
        for item in required_artifacts:
            spec = item if isinstance(item, dict) else {"relative_path": item, "expectation": "locked"}
            relative_path = spec["relative_path"]
            expectation = spec.get("expectation", "locked")
            row = rows_by_path.get(relative_path)
            if expectation == "needs_confirmation":
                continue
            if row is None:
                raise ValueError(f"required artifact not present in manifest {manifest_path}: {relative_path}")
            if row.get("status") != "locked":
                raise ValueError(
                    f"required artifact not locked in manifest {manifest_path}: {relative_path} status={row.get('status')}"
                )

    print("matrix validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
