#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str((Path(sys.argv[1]).resolve() / "build" / "python-deps")))
sys.path.insert(0, str((Path(sys.argv[1]).resolve() / "tools")))

from moex_phase0_common import load_json_yaml  # noqa: E402
from plaza2_schema_common import stable_numeric_id  # noqa: E402


REQUIRED_PRIVATE_STREAMS = {
    "FORTS_TRADE_REPL",
    "FORTS_USERORDERBOOK_REPL",
    "FORTS_POS_REPL",
    "FORTS_PART_REPL",
    "FORTS_REFDATA_REPL",
    "FORTS_SESSIONSTATE_REPL",
    "FORTS_INSTRUMENTSTATE_REPL",
    "FORTS_SECURITYGROUPSTATE_REPL",
}


def assert_unique(name: str, values: list[int]) -> None:
    if len(values) != len(set(values)):
        raise SystemExit(f"duplicate numeric ids detected in {name}")


def main() -> int:
    project_root = Path(sys.argv[1]).resolve()

    metadata = load_json_yaml(
        project_root / "protocols" / "plaza2_cgate" / "generated" / "plaza2_generated_metadata.json"
    )
    streams_inventory = load_json_yaml(project_root / "matrix" / "protocol_inventory" / "plaza2_streams.yaml")["items"]
    tables_inventory = load_json_yaml(project_root / "matrix" / "protocol_inventory" / "plaza2_tables.yaml")["items"]
    docs_manifest = load_json_yaml(project_root / "spec-lock" / "prod" / "plaza2" / "docs" / "manifest.json")
    docs_rows = {row["relative_path"]: row for row in docs_manifest["artifacts"]}

    expected_streams = [
        item
        for item in streams_inventory
        if item.get("kind") == "stream" and item.get("scheme_filename") == "forts_scheme.ini"
    ]
    expected_tables = [item for item in tables_inventory if item.get("kind") == "table"]

    generated_streams = metadata["streams"]
    generated_tables = metadata["tables"]
    generated_fields = metadata["fields"]
    generated_types = metadata["types"]

    if metadata["schema"]["source_sha256"] != docs_rows["p2gate_en.html"]["sha256"]:
        raise SystemExit("generated metadata source sha256 drifted from locked p2gate_en.html")

    stream_protocol_ids = {row["protocol_item_id"] for row in generated_streams}
    missing_streams = sorted(
        item["protocol_item_id"]
        for item in expected_streams
        if item["protocol_item_id"] not in stream_protocol_ids
    )
    if missing_streams:
        raise SystemExit(f"generated stream metadata is missing locked Phase 3A streams: {missing_streams}")

    table_protocol_ids = {row["protocol_item_id"] for row in generated_tables}
    missing_tables = sorted(
        item["protocol_item_id"]
        for item in expected_tables
        if item["protocol_item_id"] not in table_protocol_ids
    )
    if missing_tables:
        raise SystemExit(f"generated table metadata is missing locked Phase 3A tables: {missing_tables}")

    private_streams = {
        row["stream_name"]
        for row in generated_streams
        if row["scope_bucket"] in {"private_core", "private_auxiliary"}
    }
    missing_private_streams = sorted(REQUIRED_PRIVATE_STREAMS - private_streams)
    if missing_private_streams:
        raise SystemExit(f"required private-state streams are missing from generated metadata: {missing_private_streams}")

    type_tokens = {row["type_token"] for row in generated_types}
    for required_type in {"i8", "c25", "d16.5", "t"}:
        if required_type not in type_tokens:
            raise SystemExit(f"required PLAZA II type token missing: {required_type}")

    assert_unique("streams", [row["stream_id"] for row in generated_streams])
    assert_unique("tables", [row["table_id"] for row in generated_tables])
    assert_unique("fields", [row["field_id"] for row in generated_fields])
    assert_unique("types", [row["type_id"] for row in generated_types])

    for row in generated_streams:
        expected_id = stable_numeric_id("stream", row["stream_name"])
        if row["stream_id"] != expected_id:
            raise SystemExit(f"stream id drift detected for {row['stream_name']}")

    for row in generated_tables:
        expected_id = stable_numeric_id("table", row["stream_name"], row["table_name"])
        if row["table_id"] != expected_id:
            raise SystemExit(f"table id drift detected for {row['stream_name']}.{row['table_name']}")

    for row in generated_fields:
        expected_id = stable_numeric_id("field", row["stream_name"], row["table_name"], row["field_name"])
        if row["field_id"] != expected_id:
            raise SystemExit(f"field id drift detected for {row['stream_name']}.{row['table_name']}.{row['field_name']}")

    for row in generated_types:
        expected_id = stable_numeric_id("type", row["type_token"])
        if row["type_id"] != expected_id:
            raise SystemExit(f"type id drift detected for {row['type_token']}")

    if generated_streams[0]["stream_name"] != "FORTS_TRADE_REPL":
        raise SystemExit("stream ordering drifted away from the locked Phase 3A inventory")
    if generated_tables[0]["table_name"] != "orders_log":
        raise SystemExit("table ordering drifted away from the locked Phase 3A inventory")
    if generated_fields[0]["field_name"] != "replID":
        raise SystemExit("field ordering drifted away from the locked doc surface")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
