#!/usr/bin/env python3
from __future__ import annotations

from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any

from moex_phase0_common import load_json_yaml
from plaza2_schema_common import cpp_pascal_case


ALLOWED_EVENT_TYPES = {
    "OPEN",
    "CLOSE",
    "SNAPSHOT_BEGIN",
    "SNAPSHOT_END",
    "ONLINE",
    "TN_BEGIN",
    "TN_COMMIT",
    "STREAM_DATA",
    "P2REPL_REPLSTATE",
    "P2REPL_LIFENUM",
    "P2REPL_CLEARDELETED",
}

ALLOWED_INVARIANT_TYPES = {
    "commit_count",
    "last_lifenum",
    "stream_online",
    "last_replstate",
    "clear_deleted_count",
}

EVENT_KIND_ENUM = {
    "OPEN": "EventKind::kOpen",
    "CLOSE": "EventKind::kClose",
    "SNAPSHOT_BEGIN": "EventKind::kSnapshotBegin",
    "SNAPSHOT_END": "EventKind::kSnapshotEnd",
    "ONLINE": "EventKind::kOnline",
    "TN_BEGIN": "EventKind::kTransactionBegin",
    "TN_COMMIT": "EventKind::kTransactionCommit",
    "STREAM_DATA": "EventKind::kStreamData",
    "P2REPL_REPLSTATE": "EventKind::kReplState",
    "P2REPL_LIFENUM": "EventKind::kLifeNum",
    "P2REPL_CLEARDELETED": "EventKind::kClearDeleted",
}

INVARIANT_KIND_ENUM = {
    "commit_count": "InvariantKind::kCommitCount",
    "last_lifenum": "InvariantKind::kLastLifenum",
    "stream_online": "InvariantKind::kStreamOnline",
    "last_replstate": "InvariantKind::kLastReplstate",
    "clear_deleted_count": "InvariantKind::kClearDeletedCount",
}

VALUE_KIND_ENUM = {
    "none": "ValueKind::kNone",
    "signed_integer": "ValueKind::kSignedInteger",
    "unsigned_integer": "ValueKind::kUnsignedInteger",
    "decimal": "ValueKind::kDecimal",
    "floating_point": "ValueKind::kFloatingPoint",
    "string": "ValueKind::kString",
    "timestamp": "ValueKind::kTimestamp",
}


def stream_code_token(stream_name: str) -> str:
    return f"generated::StreamCode::k{cpp_pascal_case(stream_name)}"


def table_code_token(stream_name: str, table_name: str) -> str:
    return f"generated::TableCode::k{cpp_pascal_case(stream_name)}{cpp_pascal_case(table_name)}"


def field_code_token(stream_name: str, table_name: str, field_name: str) -> str:
    return (
        "generated::FieldCode::k"
        f"{cpp_pascal_case(stream_name)}{cpp_pascal_case(table_name)}{cpp_pascal_case(field_name)}"
    )


def _expect(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _expect_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{label} must be a non-empty string")
    return value.strip()


def _expect_uint(value: Any, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{label} must be a non-negative integer")
    return value


def _canonical_number_text(value: Any, label: str) -> str:
    if isinstance(value, bool):
        raise ValueError(f"{label} must be numeric")
    if isinstance(value, (int, float, str)):
        text = str(value).strip()
        try:
            normalized = Decimal(text)
        except InvalidOperation as error:
            raise ValueError(f"{label} must be numeric") from error
        if not normalized.is_finite():
            raise ValueError(f"{label} must be finite")
        rendered = format(normalized, "f")
        if "." in rendered:
            rendered = rendered.rstrip("0").rstrip(".") or "0"
        if rendered == "-0":
            rendered = "0"
        return rendered
    raise ValueError(f"{label} must be numeric")


def load_plaza2_metadata_index(metadata_path: Path) -> dict:
    metadata = load_json_yaml(metadata_path)
    streams_by_name: dict[str, dict] = {}
    tables_by_key: dict[tuple[str, str], dict] = {}
    fields_by_key: dict[tuple[str, str, str], dict] = {}
    fields_by_table: dict[tuple[str, str], list[dict]] = {}
    required_fields_by_table: dict[tuple[str, str], list[str]] = {}

    for row in metadata["streams"]:
        streams_by_name[row["stream_name"]] = row

    for row in metadata["tables"]:
        tables_by_key[(row["stream_name"], row["table_name"])] = row

    for row in metadata["fields"]:
        key = (row["stream_name"], row["table_name"])
        fields_by_key[(row["stream_name"], row["table_name"], row["field_name"])] = row
        fields_by_table.setdefault(key, []).append(row)

    for key, rows in fields_by_table.items():
        field_names = {row["field_name"] for row in rows}
        required_fields_by_table[key] = ["replID"] if "replID" in field_names else []

    return {
        "raw": metadata,
        "streams_by_name": streams_by_name,
        "tables_by_key": tables_by_key,
        "fields_by_key": fields_by_key,
        "fields_by_table": fields_by_table,
        "required_fields_by_table": required_fields_by_table,
    }


def _parse_field_value(field_meta: dict, value: Any, label: str) -> dict:
    value_class = field_meta["value_class"]
    if value_class == "signed_integer":
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError(f"{label} must be an integer")
        return {
            "kind": "signed_integer",
            "signed_value": value,
            "unsigned_value": 0,
            "text_value": str(value),
        }
    if value_class == "unsigned_integer":
        unsigned_value = _expect_uint(value, label)
        return {
            "kind": "unsigned_integer",
            "signed_value": 0,
            "unsigned_value": unsigned_value,
            "text_value": str(unsigned_value),
        }
    if value_class == "timestamp":
        unsigned_value = _expect_uint(value, label)
        return {
            "kind": "timestamp",
            "signed_value": 0,
            "unsigned_value": unsigned_value,
            "text_value": str(unsigned_value),
        }
    if value_class in {"fixed_string", "binary"}:
        return {
            "kind": "string",
            "signed_value": 0,
            "unsigned_value": 0,
            "text_value": _expect_string(value, label),
        }
    if value_class == "decimal":
        return {
            "kind": "decimal",
            "signed_value": 0,
            "unsigned_value": 0,
            "text_value": _canonical_number_text(value, label),
        }
    if value_class == "floating_point":
        return {
            "kind": "floating_point",
            "signed_value": 0,
            "unsigned_value": 0,
            "text_value": _canonical_number_text(value, label),
        }
    raise ValueError(f"{label} uses unsupported Phase 3D value class {value_class!r}")


def _parse_streams(scenario_path: Path, scenario: dict, metadata_index: dict) -> list[dict]:
    streams = scenario.get("streams")
    if not isinstance(streams, list) or not streams:
        raise ValueError(f"{scenario_path.name}: streams must be a non-empty list")

    parsed: list[dict] = []
    seen_names: set[str] = set()
    for index, item in enumerate(streams, start=1):
        if not isinstance(item, dict):
            raise ValueError(f"{scenario_path.name}: streams[{index}] must be a mapping")
        name = _expect_string(item.get("name"), f"{scenario_path.name}: streams[{index}].name")
        if name in seen_names:
            raise ValueError(f"{scenario_path.name}: duplicate stream {name!r}")
        stream_meta = metadata_index["streams_by_name"].get(name)
        if stream_meta is None:
            raise ValueError(f"{scenario_path.name}: unknown stream {name!r}")
        scope = item.get("scope")
        if scope is not None and scope != stream_meta["scope_bucket"]:
            raise ValueError(
                f"{scenario_path.name}: stream {name!r} scope {scope!r} "
                f"does not match metadata scope {stream_meta['scope_bucket']!r}"
            )
        parsed.append(
            {
                "stream_name": name,
                "scope_bucket": stream_meta["scope_bucket"],
                "stream_code_token": stream_code_token(name),
                "stream_id": stream_meta["stream_id"],
            }
        )
        seen_names.add(name)
    return parsed


def _parse_rows(
    scenario_path: Path,
    event_index: int,
    stream_name: str,
    table_name: str,
    rows: Any,
    metadata_index: dict,
) -> list[dict]:
    if not isinstance(rows, list) or not rows:
        raise ValueError(f"{scenario_path.name}: events[{event_index}].rows must be a non-empty list")
    table_key = (stream_name, table_name)
    required_fields = metadata_index["required_fields_by_table"][table_key]
    parsed_rows: list[dict] = []

    for row_index, row in enumerate(rows, start=1):
        if not isinstance(row, dict) or not row:
            raise ValueError(f"{scenario_path.name}: row {row_index} in events[{event_index}] must be a non-empty mapping")
        for field_name in required_fields:
            if field_name not in row:
                raise ValueError(
                    f"{scenario_path.name}: row {row_index} in events[{event_index}] "
                    f"is missing required field {field_name!r}"
                )

        parsed_fields: list[dict] = []
        for field_name, value in row.items():
            field_meta = metadata_index["fields_by_key"].get((stream_name, table_name, field_name))
            if field_meta is None:
                raise ValueError(
                    f"{scenario_path.name}: row {row_index} in events[{event_index}] "
                    f"references unknown field {field_name!r} for {stream_name}.{table_name}"
                )
            value_spec = _parse_field_value(
                field_meta,
                value,
                f"{scenario_path.name}: row {row_index} field {field_name!r} in events[{event_index}]",
            )
            parsed_fields.append(
                {
                    "field_name": field_name,
                    "field_code_token": field_code_token(stream_name, table_name, field_name),
                    "value_kind": value_spec["kind"],
                    "signed_value": value_spec["signed_value"],
                    "unsigned_value": value_spec["unsigned_value"],
                    "text_value": value_spec["text_value"],
                }
            )

        parsed_rows.append(
            {
                "stream_name": stream_name,
                "table_name": table_name,
                "stream_code_token": stream_code_token(stream_name),
                "table_code_token": table_code_token(stream_name, table_name),
                "fields": parsed_fields,
            }
        )
    return parsed_rows


def _validate_event_sequence(scenario_path: Path, events: list[dict]) -> None:
    _expect(events, f"{scenario_path.name}: events must be non-empty")
    open_seen = False
    close_seen = False
    snapshot_active = False
    snapshot_completed = False
    transaction_active = False

    for index, event in enumerate(events, start=1):
        kind = event["type"]
        if close_seen:
            raise ValueError(f"{scenario_path.name}: events[{index}] appears after CLOSE")
        if index == 1 and kind != "OPEN":
            raise ValueError(f"{scenario_path.name}: first event must be OPEN")

        if kind == "OPEN":
            if open_seen:
                raise ValueError(f"{scenario_path.name}: OPEN may appear only once")
            open_seen = True
            continue

        if not open_seen:
            raise ValueError(f"{scenario_path.name}: events[{index}] requires OPEN first")

        if kind == "CLOSE":
            if transaction_active:
                raise ValueError(f"{scenario_path.name}: CLOSE is invalid while a transaction is open")
            close_seen = True
        elif kind == "SNAPSHOT_BEGIN":
            if transaction_active:
                raise ValueError(f"{scenario_path.name}: SNAPSHOT_BEGIN is invalid inside a transaction")
            if snapshot_active:
                raise ValueError(f"{scenario_path.name}: SNAPSHOT_BEGIN may not nest")
            snapshot_active = True
            snapshot_completed = False
        elif kind == "SNAPSHOT_END":
            if transaction_active:
                raise ValueError(f"{scenario_path.name}: SNAPSHOT_END is invalid inside a transaction")
            if not snapshot_active:
                raise ValueError(f"{scenario_path.name}: SNAPSHOT_END requires SNAPSHOT_BEGIN")
            snapshot_active = False
            snapshot_completed = True
        elif kind == "ONLINE":
            if transaction_active:
                raise ValueError(f"{scenario_path.name}: ONLINE is invalid inside a transaction")
            if snapshot_active:
                raise ValueError(f"{scenario_path.name}: ONLINE requires SNAPSHOT_END before it")
            if not snapshot_completed:
                raise ValueError(f"{scenario_path.name}: ONLINE requires a completed snapshot")
        elif kind == "TN_BEGIN":
            if transaction_active:
                raise ValueError(f"{scenario_path.name}: nested TN_BEGIN is invalid")
            transaction_active = True
        elif kind == "TN_COMMIT":
            if not transaction_active:
                raise ValueError(f"{scenario_path.name}: TN_COMMIT requires TN_BEGIN")
            transaction_active = False
        elif kind == "STREAM_DATA":
            if not transaction_active:
                raise ValueError(f"{scenario_path.name}: STREAM_DATA outside TN_BEGIN/TN_COMMIT is invalid")
        elif kind in {"P2REPL_REPLSTATE", "P2REPL_LIFENUM", "P2REPL_CLEARDELETED"}:
            if transaction_active:
                raise ValueError(f"{scenario_path.name}: {kind} is invalid inside a transaction")

    if transaction_active:
        raise ValueError(f"{scenario_path.name}: scenario ends with an open transaction")


def parse_plaza2_fake_scenario(scenario_path: Path, metadata_index: dict) -> dict:
    loaded = load_json_yaml(scenario_path)
    if not isinstance(loaded, dict):
        raise ValueError(f"{scenario_path.name}: root must be a mapping")

    scenario_id = _expect_string(loaded.get("scenario_id"), f"{scenario_path.name}: scenario_id")
    if scenario_id != scenario_path.stem:
        raise ValueError(f"{scenario_path.name}: scenario_id must match filename stem {scenario_path.stem!r}")
    description = _expect_string(loaded.get("description"), f"{scenario_path.name}: description")

    metadata = loaded.get("metadata")
    if not isinstance(metadata, dict):
        raise ValueError(f"{scenario_path.name}: metadata must be a mapping")
    metadata_version = _expect_uint(metadata.get("version"), f"{scenario_path.name}: metadata.version")
    if metadata_version != 1:
        raise ValueError(f"{scenario_path.name}: unsupported metadata.version {metadata_version}")
    deterministic_seed = metadata.get("deterministic_seed", 0)
    deterministic_seed = _expect_uint(deterministic_seed, f"{scenario_path.name}: metadata.deterministic_seed")

    parsed_streams = _parse_streams(scenario_path, loaded, metadata_index)
    declared_streams = {row["stream_name"] for row in parsed_streams}

    events = loaded.get("events")
    if not isinstance(events, list) or not events:
        raise ValueError(f"{scenario_path.name}: events must be a non-empty list")
    parsed_events: list[dict] = []
    for event_index, event in enumerate(events, start=1):
        if not isinstance(event, dict):
            raise ValueError(f"{scenario_path.name}: events[{event_index}] must be a mapping")
        event_type = _expect_string(event.get("type"), f"{scenario_path.name}: events[{event_index}].type")
        if event_type not in ALLOWED_EVENT_TYPES:
            raise ValueError(f"{scenario_path.name}: unsupported event type {event_type!r}")

        parsed_event = {
            "type": event_type,
            "kind_enum": EVENT_KIND_ENUM[event_type],
            "stream_name": "",
            "table_name": "",
            "stream_code_token": "kNoStreamCode",
            "table_code_token": "kNoTableCode",
            "rows": [],
            "numeric_value": 0,
            "text_value": "",
        }

        if event_type == "STREAM_DATA":
            stream_name = _expect_string(event.get("stream"), f"{scenario_path.name}: events[{event_index}].stream")
            if stream_name not in declared_streams:
                raise ValueError(
                    f"{scenario_path.name}: events[{event_index}] references undeclared stream {stream_name!r}"
                )
            table_name = _expect_string(event.get("table"), f"{scenario_path.name}: events[{event_index}].table")
            table_meta = metadata_index["tables_by_key"].get((stream_name, table_name))
            if table_meta is None:
                raise ValueError(
                    f"{scenario_path.name}: events[{event_index}] references unknown table "
                    f"{stream_name}.{table_name}"
                )
            parsed_event["stream_name"] = stream_name
            parsed_event["table_name"] = table_name
            parsed_event["stream_code_token"] = stream_code_token(stream_name)
            parsed_event["table_code_token"] = table_code_token(stream_name, table_name)
            parsed_event["rows"] = _parse_rows(
                scenario_path, event_index, stream_name, table_name, event.get("rows"), metadata_index
            )
        elif event_type == "P2REPL_REPLSTATE":
            parsed_event["text_value"] = _expect_string(
                event.get("value"), f"{scenario_path.name}: events[{event_index}].value"
            )
        elif event_type == "P2REPL_LIFENUM":
            parsed_event["numeric_value"] = _expect_uint(
                event.get("value"), f"{scenario_path.name}: events[{event_index}].value"
            )
        elif event_type == "P2REPL_CLEARDELETED":
            stream_name = _expect_string(event.get("stream"), f"{scenario_path.name}: events[{event_index}].stream")
            if stream_name not in declared_streams:
                raise ValueError(
                    f"{scenario_path.name}: events[{event_index}] references undeclared stream {stream_name!r}"
                )
            parsed_event["stream_name"] = stream_name
            parsed_event["stream_code_token"] = stream_code_token(stream_name)

        parsed_events.append(parsed_event)

    _validate_event_sequence(scenario_path, parsed_events)

    expected = loaded.get("expected", {})
    if not isinstance(expected, dict):
        raise ValueError(f"{scenario_path.name}: expected must be a mapping")
    invariants = expected.get("invariants", [])
    if not isinstance(invariants, list):
        raise ValueError(f"{scenario_path.name}: expected.invariants must be a list")
    parsed_invariants: list[dict] = []
    for invariant_index, invariant in enumerate(invariants, start=1):
        if not isinstance(invariant, dict):
            raise ValueError(f"{scenario_path.name}: invariant {invariant_index} must be a mapping")
        invariant_type = _expect_string(
            invariant.get("type"), f"{scenario_path.name}: expected.invariants[{invariant_index}].type"
        )
        if invariant_type not in ALLOWED_INVARIANT_TYPES:
            raise ValueError(f"{scenario_path.name}: unsupported invariant type {invariant_type!r}")
        parsed_invariant = {
            "type": invariant_type,
            "kind_enum": INVARIANT_KIND_ENUM[invariant_type],
            "stream_name": "",
            "stream_code_token": "kNoStreamCode",
            "numeric_value": 0,
            "bool_value": False,
            "text_value": "",
        }
        if invariant_type in {"commit_count", "last_lifenum"}:
            parsed_invariant["numeric_value"] = _expect_uint(
                invariant.get("value"),
                f"{scenario_path.name}: expected.invariants[{invariant_index}].value",
            )
        elif invariant_type == "stream_online":
            stream_name = _expect_string(
                invariant.get("stream"),
                f"{scenario_path.name}: expected.invariants[{invariant_index}].stream",
            )
            if stream_name not in declared_streams:
                raise ValueError(
                    f"{scenario_path.name}: invariant {invariant_index} references undeclared stream {stream_name!r}"
                )
            if not isinstance(invariant.get("value"), bool):
                raise ValueError(
                    f"{scenario_path.name}: expected.invariants[{invariant_index}].value must be a boolean"
                )
            parsed_invariant["stream_name"] = stream_name
            parsed_invariant["stream_code_token"] = stream_code_token(stream_name)
            parsed_invariant["bool_value"] = invariant["value"]
        elif invariant_type == "last_replstate":
            parsed_invariant["text_value"] = _expect_string(
                invariant.get("value"),
                f"{scenario_path.name}: expected.invariants[{invariant_index}].value",
            )
        elif invariant_type == "clear_deleted_count":
            stream_name = _expect_string(
                invariant.get("stream"),
                f"{scenario_path.name}: expected.invariants[{invariant_index}].stream",
            )
            if stream_name not in declared_streams:
                raise ValueError(
                    f"{scenario_path.name}: invariant {invariant_index} references undeclared stream {stream_name!r}"
                )
            parsed_invariant["stream_name"] = stream_name
            parsed_invariant["stream_code_token"] = stream_code_token(stream_name)
            parsed_invariant["numeric_value"] = _expect_uint(
                invariant.get("value"),
                f"{scenario_path.name}: expected.invariants[{invariant_index}].value",
            )

        parsed_invariants.append(parsed_invariant)

    return {
        "scenario_id": scenario_id,
        "description": description,
        "metadata_version": metadata_version,
        "deterministic_seed": deterministic_seed,
        "streams": parsed_streams,
        "events": parsed_events,
        "invariants": parsed_invariants,
    }


def load_plaza2_fake_scenarios(scenarios_dir: Path, metadata_index: dict) -> list[dict]:
    scenario_paths = sorted(scenarios_dir.glob("*.yaml"))
    if not scenario_paths:
        raise ValueError(f"no Phase 3D fake-engine scenarios were found under {scenarios_dir}")

    scenarios: list[dict] = []
    seen_ids: set[str] = set()
    for scenario_path in scenario_paths:
        parsed = parse_plaza2_fake_scenario(scenario_path, metadata_index)
        if parsed["scenario_id"] in seen_ids:
            raise ValueError(f"duplicate scenario_id {parsed['scenario_id']!r}")
        seen_ids.add(parsed["scenario_id"])
        scenarios.append(parsed)
    return scenarios
