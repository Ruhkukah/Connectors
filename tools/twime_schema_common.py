#!/usr/bin/env python3
from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any
import xml.etree.ElementTree as ET

from moex_phase0_common import load_json_yaml, sha256_file, stable_id


SBE_NS = {"sbe": "http://fixprotocol.io/2016/sbe"}
PRIMITIVE_SIZES = {
    "int8": 1,
    "int16": 2,
    "int32": 4,
    "int64": 8,
    "uint8": 1,
    "uint16": 2,
    "uint32": 4,
    "uint64": 8,
    "char": 1,
}


@dataclass(frozen=True)
class ArtifactContext:
    artifact_id: str
    canonical_url: str
    sha256: str
    size: int
    retrieved_at: str
    schema_id: int
    schema_version: int
    byte_order: str
    package: str


def parse_scalar_value(text: str, primitive_type: str) -> int:
    stripped = (text or "").strip()
    if primitive_type == "char":
        if not stripped:
            return 0
        return ord(stripped[0])
    if stripped.lower().startswith("0x"):
        return int(stripped, 16)
    return int(stripped)


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def load_artifact_context(schema_path: Path) -> ArtifactContext:
    schema_manifest_path = schema_path.parent / "schema.manifest.json"
    if schema_manifest_path.exists():
        payload = load_json_yaml(schema_manifest_path)
        return ArtifactContext(
            artifact_id=payload["source_artifact_id"],
            canonical_url=payload["canonical_url"],
            sha256=payload["sha256"],
            size=int(payload["size"]),
            retrieved_at=payload["retrieved_at"],
            schema_id=int(payload["schema_id"]),
            schema_version=int(payload["schema_version"]),
            byte_order=str(payload["byte_order"]),
            package=str(payload["package"]),
        )

    return ArtifactContext(
        artifact_id="twime_prod_root",
        canonical_url=schema_path.resolve().as_uri(),
        sha256=sha256_file(schema_path),
        size=schema_path.stat().st_size,
        retrieved_at="unknown",
        schema_id=0,
        schema_version=0,
        byte_order="unknown",
        package="unknown",
    )


def _direction_for_message(message_name: str, template_id: int) -> str:
    if 6000 <= template_id < 7000:
        return "client_to_server"
    if 7000 <= template_id < 8000:
        return "server_to_client"

    session_directions = {
        "Establish": "client_to_server",
        "EstablishmentAck": "server_to_client",
        "EstablishmentReject": "server_to_client",
        "Terminate": "bidirectional",
        "RetransmitRequest": "client_to_server",
        "Retransmission": "server_to_client",
        "Sequence": "bidirectional",
        "FloodReject": "server_to_client",
        "SessionReject": "server_to_client",
        "BusinessMessageReject": "server_to_client",
    }
    return session_directions.get(message_name, "unknown")


def _layer_for_template_id(template_id: int) -> str:
    if 5000 <= template_id < 6000:
        return "session"
    return "application"


def _normalize_nullable(type_entry: dict[str, Any], field_attrs: dict[str, str]) -> tuple[bool, int | None]:
    presence = field_attrs.get("presence", type_entry.get("presence", "required"))
    nullable = presence == "optional" or type_entry.get("null_value") is not None
    null_value = type_entry.get("null_value")
    return nullable, null_value


def _parse_types(root: ET.Element) -> tuple[dict[str, dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]]:
    types_node = root.find("types")
    if types_node is None:
        raise ValueError("TWIME schema is missing <types> section")

    types_by_name: dict[str, dict[str, Any]] = {}
    type_items: list[dict[str, Any]] = []
    enum_items: list[dict[str, Any]] = []
    field_type_rows: list[dict[str, Any]] = []

    for type_node in types_node:
        tag = type_node.tag.split("}")[-1]
        name = type_node.attrib["name"]
        if tag == "type":
            primitive_type = type_node.attrib["primitiveType"]
            fixed_length = int(type_node.attrib.get("length", "1"))
            encoded_size = PRIMITIVE_SIZES[primitive_type] * fixed_length
            type_kind = "string" if primitive_type == "char" and fixed_length > 1 else "primitive"
            if name == "DeltaMillisecs":
                type_kind = "delta_millisecs"
            elif name == "TimeStamp":
                type_kind = "timestamp"
            null_value = type_node.attrib.get("nullValue")
            type_entry = {
                "name": name,
                "kind": type_kind,
                "primitive_type": primitive_type,
                "encoded_size": encoded_size,
                "presence": type_node.attrib.get("presence", "required"),
                "fixed_length": fixed_length if type_kind == "string" else 0,
                "null_value": None if null_value is None else parse_scalar_value(null_value, primitive_type),
                "description": type_node.attrib.get("description", ""),
                "min_value": type_node.attrib.get("minValue"),
                "max_value": type_node.attrib.get("maxValue"),
            }
            types_by_name[name] = type_entry
            type_items.append(
                {
                    "protocol_item_id": stable_id("twime_type", name),
                    "artifact_id": "",
                    "schema_id": 0,
                    "schema_version": 0,
                    "type_name": name,
                    "kind": type_kind,
                    "primitive_type": primitive_type,
                    "encoded_size": encoded_size,
                    "fixed_length": type_entry["fixed_length"] or None,
                    "nullable": type_entry["presence"] == "optional" or type_entry["null_value"] is not None,
                    "null_value": type_entry["null_value"],
                    "implementation_status": "metadata_generated",
                    "notes": type_entry["description"],
                }
            )
            continue

        if tag == "enum":
            encoding_type = type_node.attrib["encodingType"]
            values = []
            for value_node in type_node:
                wire_text = (value_node.text or "").strip()
                values.append(
                    {
                        "name": value_node.attrib["name"],
                        "wire_text": wire_text,
                        "wire_value": parse_scalar_value(wire_text, encoding_type) if wire_text else 0,
                    }
                )
            encoded_size = PRIMITIVE_SIZES[encoding_type]
            type_entry = {
                "name": name,
                "kind": "enum",
                "primitive_type": encoding_type,
                "encoded_size": encoded_size,
                "presence": type_node.attrib.get("presence", "required"),
                "fixed_length": 0,
                "null_value": None,
                "values": values,
                "description": type_node.attrib.get("description", ""),
            }
            types_by_name[name] = type_entry
            enum_items.append(
                {
                    "protocol_item_id": stable_id("twime_enum", name),
                    "artifact_id": "",
                    "schema_id": 0,
                    "schema_version": 0,
                    "enum_name": name,
                    "encoding_type": encoding_type,
                    "encoded_size": encoded_size,
                    "values": values,
                    "implementation_status": "metadata_generated",
                    "notes": type_entry["description"],
                }
            )
            continue

        if tag == "set":
            encoding_type = type_node.attrib["encodingType"]
            choices = []
            for choice_node in type_node:
                bit_index = int((choice_node.text or "0").strip())
                choices.append(
                    {
                        "name": choice_node.attrib["name"],
                        "bit_index": bit_index,
                        "mask": str(1 << bit_index),
                        "description": choice_node.attrib.get("description", ""),
                    }
                )
            encoded_size = PRIMITIVE_SIZES[encoding_type]
            type_entry = {
                "name": name,
                "kind": "set",
                "primitive_type": encoding_type,
                "encoded_size": encoded_size,
                "presence": type_node.attrib.get("presence", "required"),
                "fixed_length": 0,
                "null_value": None,
                "choices": choices,
                "description": type_node.attrib.get("description", ""),
            }
            types_by_name[name] = type_entry
            enum_items.append(
                {
                    "protocol_item_id": stable_id("twime_set", name),
                    "artifact_id": "",
                    "schema_id": 0,
                    "schema_version": 0,
                    "enum_name": name,
                    "encoding_type": encoding_type,
                    "encoded_size": encoded_size,
                    "values": choices,
                    "implementation_status": "metadata_generated",
                    "notes": type_entry["description"],
                }
            )
            continue

        if tag == "composite":
            encoded_size = 0
            constant_exponent = None
            primitive_type = "composite"
            members = []
            for child in type_node:
                child_tag = child.tag.split("}")[-1]
                if child_tag != "type":
                    continue
                child_primitive = child.attrib["primitiveType"]
                child_presence = child.attrib.get("presence", "required")
                wire_text = child.text.strip() if child.text else ""
                if child_presence != "constant":
                    encoded_size += PRIMITIVE_SIZES[child_primitive]
                elif child.attrib["name"] == "exponent":
                    constant_exponent = parse_scalar_value(wire_text, child_primitive)
                members.append(
                    {
                        "name": child.attrib["name"],
                        "primitive_type": child_primitive,
                        "presence": child_presence,
                        "wire_text": wire_text,
                    }
                )
            type_kind = "decimal5" if name == "Decimal5" else "composite"
            type_entry = {
                "name": name,
                "kind": type_kind,
                "primitive_type": primitive_type,
                "encoded_size": encoded_size,
                "presence": type_node.attrib.get("presence", "required"),
                "fixed_length": 0,
                "null_value": None,
                "constant_exponent": constant_exponent,
                "members": members,
                "description": type_node.attrib.get("description", ""),
            }
            types_by_name[name] = type_entry
            type_items.append(
                {
                    "protocol_item_id": stable_id("twime_type", name),
                    "artifact_id": "",
                    "schema_id": 0,
                    "schema_version": 0,
                    "type_name": name,
                    "kind": type_kind,
                    "primitive_type": primitive_type,
                    "encoded_size": encoded_size,
                    "fixed_length": None,
                    "nullable": False,
                    "null_value": None,
                    "implementation_status": "metadata_generated",
                    "notes": type_entry["description"],
                }
            )
            continue

        field_type_rows.append({"unsupported_type_tag": tag, "name": name})

    return types_by_name, type_items, enum_items, field_type_rows


def parse_twime_schema(schema_path: Path) -> dict[str, Any]:
    artifact = load_artifact_context(schema_path)
    tree = ET.parse(schema_path)
    root = tree.getroot()
    package = root.attrib["package"]
    schema_id = int(root.attrib["id"])
    schema_version = int(root.attrib["version"])
    byte_order = root.attrib["byteOrder"]

    types_by_name, type_items, enum_items, _ = _parse_types(root)
    messages = []
    flat_fields = []

    for type_row in type_items:
        type_row["artifact_id"] = artifact.artifact_id
        type_row["schema_id"] = schema_id
        type_row["schema_version"] = schema_version
    for enum_row in enum_items:
        enum_row["artifact_id"] = artifact.artifact_id
        enum_row["schema_id"] = schema_id
        enum_row["schema_version"] = schema_version

    for message_node in root.findall("sbe:message", SBE_NS):
        message_name = message_node.attrib["name"]
        template_id = int(message_node.attrib["id"])
        layer = _layer_for_template_id(template_id)
        direction = _direction_for_message(message_name, template_id)
        block_length = 0
        message_fields = []

        for field_node in message_node:
            if field_node.tag.split("}")[-1] != "field":
                raise ValueError(f"Unsupported TWIME node under message {message_name}: {field_node.tag}")

            field_name = field_node.attrib["name"]
            field_id = int(field_node.attrib["id"])
            type_name = field_node.attrib["type"]
            type_entry = types_by_name[type_name]
            encoded_size = int(type_entry["encoded_size"])
            nullable, null_value = _normalize_nullable(type_entry, field_node.attrib)
            field_row = {
                "protocol_item_id": stable_id("twime_field", message_name, field_name),
                "artifact_id": artifact.artifact_id,
                "schema_id": schema_id,
                "schema_version": schema_version,
                "message_name": message_name,
                "template_id": template_id,
                "field_id": field_id,
                "field_name": field_name,
                "type_name": type_name,
                "kind": type_entry["kind"],
                "primitive_type": type_entry["primitive_type"],
                "encoded_size": encoded_size,
                "offset": block_length,
                "nullable": nullable,
                "null_value": null_value,
                "implementation_status": "metadata_generated",
                "notes": type_entry.get("description", ""),
            }
            block_length += encoded_size
            message_fields.append(
                {
                    "tag": field_id,
                    "name": field_name,
                    "type": type_name,
                    "primitive_type": type_entry["primitive_type"],
                    "encoded_size": encoded_size,
                    "offset": field_row["offset"],
                    "nullable": nullable,
                    "null_value": null_value,
                }
            )
            flat_fields.append(field_row)

        messages.append(
            {
                "protocol_item_id": stable_id("twime_message", f"{template_id}", message_name),
                "artifact_id": artifact.artifact_id,
                "schema_id": schema_id,
                "schema_version": schema_version,
                "template_id": template_id,
                "message_name": message_name,
                "layer": layer,
                "direction": direction,
                "block_length": block_length,
                "fields": message_fields,
                "implementation_status": "metadata_generated",
                "notes": "",
            }
        )

    return {
        "schema": {
            "artifact_id": artifact.artifact_id,
            "canonical_url": artifact.canonical_url,
            "sha256": artifact.sha256,
            "size": artifact.size,
            "retrieved_at": artifact.retrieved_at,
            "schema_id": schema_id,
            "schema_version": schema_version,
            "byte_order": byte_order,
            "package": package,
        },
        "types": type_items,
        "enums": enum_items,
        "messages": messages,
        "fields": flat_fields,
        "types_by_name": types_by_name,
    }
