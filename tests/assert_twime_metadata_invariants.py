#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str((Path(sys.argv[1]).resolve() / "build" / "python-deps")))
sys.path.insert(0, str((Path(sys.argv[1]).resolve() / "tools")))

from moex_phase0_common import load_json_yaml  # noqa: E402
from twime_schema_common import parse_twime_schema  # noqa: E402


REQUIRED_TEMPLATE_IDS = {
    5000, 5001, 5002, 5003, 5004, 5005, 5006, 5007, 5008, 5009,
    6000, 6004, 6005, 6006, 6007, 6008, 6009, 6010, 6011,
    7007, 7010, 7014, 7015, 7016, 7017, 7018, 7019, 7020,
}


def main() -> int:
    project_root = Path(sys.argv[1]).resolve()
    schema_path = project_root / "protocols" / "twime_sbe" / "schema" / "twime_spectra-7.7.xml"
    parsed = parse_twime_schema(schema_path)
    messages_yaml = load_json_yaml(project_root / "matrix" / "protocol_inventory" / "twime_messages.yaml")["items"]
    fields_yaml = load_json_yaml(project_root / "matrix" / "protocol_inventory" / "twime_fields.yaml")["items"]
    types_yaml = load_json_yaml(project_root / "matrix" / "protocol_inventory" / "twime_types.yaml")["items"]
    enums_yaml = load_json_yaml(project_root / "matrix" / "protocol_inventory" / "twime_enums.yaml")["items"]

    schema = parsed["schema"]
    if schema["schema_id"] != 19781:
        raise SystemExit("unexpected TWIME schema id")
    if schema["schema_version"] != 7:
        raise SystemExit("unexpected TWIME schema version")
    if schema["byte_order"] != "littleEndian":
        raise SystemExit("unexpected TWIME byte order")

    template_ids = [item["template_id"] for item in messages_yaml]
    if len(template_ids) != len(set(template_ids)):
        raise SystemExit("duplicate TWIME template_id detected")

    names = [item["message_name"] for item in messages_yaml]
    if len(names) != len(set(names)):
        raise SystemExit("duplicate TWIME message name detected")

    missing_required = sorted(REQUIRED_TEMPLATE_IDS - set(template_ids))
    if missing_required:
        raise SystemExit(f"missing required TWIME template ids: {missing_required}")

    messages_by_id = {item["template_id"]: item for item in messages_yaml}
    parsed_by_id = {item["template_id"]: item for item in parsed["messages"]}
    for template_id, parsed_message in parsed_by_id.items():
        yaml_message = messages_by_id.get(template_id)
        if yaml_message is None:
            raise SystemExit(f"message inventory missing template id {template_id}")
        if yaml_message["block_length"] != parsed_message["block_length"]:
            raise SystemExit(f"block length mismatch for template {template_id}")
        for expected_field, actual_field in zip(parsed_message["fields"], yaml_message["fields"], strict=True):
            if expected_field["offset"] != actual_field["offset"]:
                raise SystemExit(f"field offset mismatch for template {template_id} field {expected_field['name']}")

    known_types = {item["type_name"] for item in types_yaml}
    known_enums_or_sets = {item["enum_name"] for item in enums_yaml}
    for field in fields_yaml:
        type_name = field["type_name"]
        if type_name not in known_types and type_name not in known_enums_or_sets:
            raise SystemExit(f"unknown field type reference: {type_name}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
