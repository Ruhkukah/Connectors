#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from moex_phase0_common import load_json_yaml


REQUIRED_COMMANDS = {
    "AddOrder",
    "IcebergAddOrder",
    "DelOrder",
    "IcebergDelOrder",
    "MoveOrder",
    "IcebergMoveOrder",
    "DelUserOrders",
    "DelOrdersByBFLimit",
    "CODHeartbeat",
}

REQUIRED_OPERATION_TYPES = {
    "add_order",
    "add_iceberg_order",
    "cancel_order",
    "cancel_iceberg_order",
    "replace_or_move_order",
    "replace_or_move_iceberg_order",
    "mass_cancel_by_mask",
    "mass_cancel_by_broker_firm_limit",
    "cancel_on_disconnect_heartbeat",
}

REQUIRED_REPLY_NAMES = {
    "FORTS_MSG176",
    "FORTS_MSG177",
    "FORTS_MSG179",
    "FORTS_MSG180",
    "FORTS_MSG181",
    "FORTS_MSG182",
    "FORTS_MSG186",
    "FORTS_MSG172",
}

REQUIRED_ERROR_NAMES = {
    "FORTS_MSG99",
    "FORTS_MSG100",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def ids(rows: list[dict]) -> set[str]:
    values = [row["protocol_item_id"] for row in rows]
    duplicates = sorted({value for value in values if values.count(value) > 1})
    require(not duplicates, f"duplicate protocol item ids: {duplicates}")
    return set(values)


def load(root: Path, relative: str) -> list[dict]:
    payload = load_json_yaml(root / relative)
    require(payload["version"] == 1, f"{relative} version mismatch")
    return payload["items"]


def metadata_tables(root: Path) -> set[str]:
    metadata = load_json_yaml(root / "protocols/plaza2_cgate/generated/plaza2_generated_metadata.json")
    return {f"{row['stream_name']}.{row['table_name']}" for row in metadata["tables"]}


def assert_no_sensitive_values(root: Path, paths: list[str]) -> None:
    forbidden = [
        "MOEX_PLAZA2_TEST_CREDENTIALS",
        "PLAZA2_TEST_CREDENTIALS",
        "password",
        "passwd",
        "secret",
        "key=",
    ]
    for relative in paths:
        text = (root / relative).read_text(encoding="utf-8").lower()
        for needle in forbidden:
            require(needle.lower() not in text, f"sensitive token {needle!r} leaked into {relative}")


def main() -> int:
    if len(sys.argv) not in {2, 3}:
        raise SystemExit("usage: assert_plaza2_trade_spec_lock.py <repo-root> [materializer]")

    root = Path(sys.argv[1]).resolve()
    materializer = Path(sys.argv[2]).resolve() if len(sys.argv) == 3 else root / "tools/plaza2_phase5a_trade_materialize.py"
    command = [str(materializer), "--project-root", str(root), "--check"]
    if materializer.suffix == ".py":
        command.insert(0, sys.executable)
    result = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    require(result.returncode == 0, result.stderr)

    command_path = "matrix/protocol_inventory/plaza2_trade_commands.yaml"
    field_path = "matrix/protocol_inventory/plaza2_trade_command_fields.yaml"
    reply_path = "matrix/protocol_inventory/plaza2_trade_replies.yaml"
    error_path = "matrix/protocol_inventory/plaza2_trade_errors.yaml"
    confirmation_path = "matrix/protocol_inventory/plaza2_trade_confirmation_map.yaml"
    manifest_path = "spec-lock/test/plaza2/trade/manifest.yaml"

    commands = load(root, command_path)
    fields = load(root, field_path)
    replies = load(root, reply_path)
    errors = load(root, error_path)
    confirmations = load(root, confirmation_path)

    command_ids = ids(commands)
    field_command_ids = {row["command_id"] for row in fields}
    reply_ids = ids(replies)
    error_ids = ids(errors)
    confirmation_command_ids = {row["command_id"] for row in confirmations}

    require({row["command_name"] for row in commands} == REQUIRED_COMMANDS, "required command coverage mismatch")
    require({row["operation_type"] for row in commands} == REQUIRED_OPERATION_TYPES, "operation type coverage mismatch")
    require({row["reply_name"] for row in replies} == REQUIRED_REPLY_NAMES, "reply coverage mismatch")
    require({row["error_name"] for row in errors} == REQUIRED_ERROR_NAMES, "error coverage mismatch")
    require(field_command_ids == command_ids, "field matrix does not cover every command")
    require(confirmation_command_ids == command_ids, "confirmation map does not cover every command")

    all_reply_like_ids = reply_ids | error_ids
    for command in commands:
        require(command["safety_classification"] == "spec_only", "command safety is not spec_only")
        require(command["runnable"] is False, "command is incorrectly marked runnable")
        require(command["reply_ids"] or command["command_name"] == "CODHeartbeat", "command missing reply linkage")
        require(command["dependencies"], "command missing dependency matrix")
        for reply_id in command["reply_ids"]:
            require(reply_id in all_reply_like_ids, f"missing reply/error reference: {reply_id}")

    table_names = metadata_tables(root)
    for row in confirmations:
        for table in row["replication_tables"]:
            require(table in table_names, f"confirmation map references unknown replication table {table}")

    for field in fields:
        require(field["command_id"] in command_ids, "field references unknown command")
        require(field["safety_classification"] == "spec_only", "field safety is not spec_only")

    manifest = load_json_yaml(root / manifest_path)
    require(manifest["raw_vendor_files_committed"] is False, "manifest must not commit raw vendor files")
    require(manifest["safety"]["live_order_sending"] is False, "manifest must keep live sending disabled")
    require(manifest["safety"]["command_builders"] is False, "manifest must keep builders disabled")
    require(manifest["safety"]["write_side_abi"] is False, "manifest must keep write-side ABI disabled")

    assert_no_sensitive_values(root, [command_path, field_path, reply_path, error_path, confirmation_path, manifest_path])
    print("Phase 5A PLAZA II trade spec lock assertions passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
