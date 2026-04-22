#!/usr/bin/env python3
from __future__ import annotations

import configparser
from dataclasses import dataclass
from pathlib import Path
import re
from typing import Any

from moex_phase0_common import stable_id


FNV1A_32_OFFSET = 0x811C9DC5
FNV1A_32_PRIME = 0x01000193

SECTION_RE = re.compile(r"^(?P<kind>stream|table|field):(?P<body>.+)$")
STRING_TYPE_RE = re.compile(r"^c(?P<length>\d+)$")
DECIMAL_TYPE_RE = re.compile(r"^d(?P<digits>\d+)\.(?P<scale>\d+)$")
BINARY_TYPE_RE = re.compile(r"^(?P<kind>b|z)(?P<length>\d+)$")
BOOL_TRUE = {"1", "true", "yes", "on"}
BOOL_FALSE = {"0", "false", "no", "off"}


@dataclass(frozen=True)
class TypeInfo:
    type_token: str
    wire_type_name: str
    value_class: str
    cpp_type: str
    cpp_type_comment: str
    storage_size_bytes: int
    logical_length: int
    decimal_digits: int
    decimal_scale: int
    sql_type: str
    cgate_type: str
    description: str


FIXED_TYPE_INFO: dict[str, TypeInfo] = {
    "a": TypeInfo(
        type_token="a",
        wire_type_name="a",
        value_class="fixed_string",
        cpp_type="char",
        cpp_type_comment="single-byte symbol string",
        storage_size_bytes=1,
        logical_length=1,
        decimal_digits=0,
        decimal_scale=0,
        sql_type="VARCHAR,1",
        cgate_type="CHAR",
        description="Symbol string, size: 1 byte.",
    ),
    "f": TypeInfo(
        type_token="f",
        wire_type_name="f",
        value_class="floating_point",
        cpp_type="double",
        cpp_type_comment="double-precision floating-point value",
        storage_size_bytes=8,
        logical_length=0,
        decimal_digits=0,
        decimal_scale=0,
        sql_type="REAL",
        cgate_type="DOUBLE",
        description="Double-precision number with floating point, size: 8 bytes.",
    ),
    "i1": TypeInfo(
        type_token="i1",
        wire_type_name="i1",
        value_class="signed_integer",
        cpp_type="std::int8_t",
        cpp_type_comment="8-bit signed integer",
        storage_size_bytes=1,
        logical_length=0,
        decimal_digits=0,
        decimal_scale=0,
        sql_type="SMALLINT",
        cgate_type="INT8",
        description="Integer with sign, size: 1 byte.",
    ),
    "i2": TypeInfo(
        type_token="i2",
        wire_type_name="i2",
        value_class="signed_integer",
        cpp_type="std::int16_t",
        cpp_type_comment="16-bit signed integer",
        storage_size_bytes=2,
        logical_length=0,
        decimal_digits=0,
        decimal_scale=0,
        sql_type="SMALLINT",
        cgate_type="INT16",
        description="Integer with sign, size: 2 bytes.",
    ),
    "i4": TypeInfo(
        type_token="i4",
        wire_type_name="i4",
        value_class="signed_integer",
        cpp_type="std::int32_t",
        cpp_type_comment="32-bit signed integer",
        storage_size_bytes=4,
        logical_length=0,
        decimal_digits=0,
        decimal_scale=0,
        sql_type="INTEGER",
        cgate_type="INT32",
        description="Integer with sign, size: 4 bytes.",
    ),
    "i8": TypeInfo(
        type_token="i8",
        wire_type_name="i8",
        value_class="signed_integer",
        cpp_type="std::int64_t",
        cpp_type_comment="64-bit signed integer",
        storage_size_bytes=8,
        logical_length=0,
        decimal_digits=0,
        decimal_scale=0,
        sql_type="BIGINT",
        cgate_type="INT64",
        description="Integer, size: 8 bytes.",
    ),
    "t": TypeInfo(
        type_token="t",
        wire_type_name="t",
        value_class="timestamp",
        cpp_type="std::int64_t",
        cpp_type_comment="timestamp value exposed as an opaque 64-bit slot until runtime adapters land",
        storage_size_bytes=0,
        logical_length=0,
        decimal_digits=0,
        decimal_scale=0,
        sql_type="TIMESTAMP",
        cgate_type="P2TIME",
        description="Date and time.",
    ),
    "u1": TypeInfo(
        type_token="u1",
        wire_type_name="u1",
        value_class="unsigned_integer",
        cpp_type="std::uint8_t",
        cpp_type_comment="8-bit unsigned integer",
        storage_size_bytes=1,
        logical_length=0,
        decimal_digits=0,
        decimal_scale=0,
        sql_type="SMALLINT",
        cgate_type="UINT8",
        description="Integer, size: 1 byte.",
    ),
    "u2": TypeInfo(
        type_token="u2",
        wire_type_name="u2",
        value_class="unsigned_integer",
        cpp_type="std::uint16_t",
        cpp_type_comment="16-bit unsigned integer",
        storage_size_bytes=2,
        logical_length=0,
        decimal_digits=0,
        decimal_scale=0,
        sql_type="INTEGER",
        cgate_type="UINT16",
        description="Integer, size: 2 bytes.",
    ),
    "u4": TypeInfo(
        type_token="u4",
        wire_type_name="u4",
        value_class="unsigned_integer",
        cpp_type="std::uint32_t",
        cpp_type_comment="32-bit unsigned integer",
        storage_size_bytes=4,
        logical_length=0,
        decimal_digits=0,
        decimal_scale=0,
        sql_type="NUMERIC,10",
        cgate_type="UINT32",
        description="Integer, size: 4 bytes.",
    ),
    "u8": TypeInfo(
        type_token="u8",
        wire_type_name="u8",
        value_class="unsigned_integer",
        cpp_type="std::uint64_t",
        cpp_type_comment="64-bit unsigned integer",
        storage_size_bytes=8,
        logical_length=0,
        decimal_digits=0,
        decimal_scale=0,
        sql_type="NUMERIC,20",
        cgate_type="UINT64",
        description="Integer, size: 8 bytes.",
    ),
}

TYPE_TOKEN_TRANSLATION = str.maketrans({
    "с": "c",
    "С": "C",
})


def fnv1a_32(text: str) -> int:
    value = FNV1A_32_OFFSET
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * FNV1A_32_PRIME) & 0xFFFFFFFF
    return value


def stable_numeric_id(*parts: str) -> int:
    return fnv1a_32("|".join(parts))


def cpp_pascal_case(raw: str) -> str:
    normalized = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", raw)
    return "".join(
        token[:1].upper() + token[1:].lower()
        for token in re.split(r"[^A-Za-z0-9]+", normalized)
        if token
    )


def cpp_string_literal(value: str) -> str:
    escaped = (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )
    return f'"{escaped}"'


def parse_bool_strict(value: str) -> bool:
    normalized = value.strip().lower()
    if normalized in BOOL_TRUE:
        return True
    if normalized in BOOL_FALSE:
        return False
    raise ValueError(f"expected boolean token, got {value!r}")


def parse_csv_list(value: str) -> list[str]:
    stripped = value.strip()
    if not stripped:
        return []
    return [item.strip() for item in stripped.split(",") if item.strip()]


def normalize_type_token(type_token: str) -> str:
    return type_token.strip().translate(TYPE_TOKEN_TRANSLATION)


def parse_type_info(type_token: str) -> TypeInfo:
    type_token = normalize_type_token(type_token)
    fixed = FIXED_TYPE_INFO.get(type_token)
    if fixed is not None:
        return fixed

    string_match = STRING_TYPE_RE.fullmatch(type_token)
    if string_match:
        logical_length = int(string_match.group("length"))
        return TypeInfo(
            type_token=type_token,
            wire_type_name="cN",
            value_class="fixed_string",
            cpp_type="std::string_view",
            cpp_type_comment="zero-terminated fixed-capacity string view",
            storage_size_bytes=logical_length + 1,
            logical_length=logical_length,
            decimal_digits=0,
            decimal_scale=0,
            sql_type=f"VARCHAR,{logical_length}",
            cgate_type="CHAR[N+1]",
            description="Symbol string, ended with zero.",
        )

    decimal_match = DECIMAL_TYPE_RE.fullmatch(type_token)
    if decimal_match:
        digits = int(decimal_match.group("digits"))
        scale = int(decimal_match.group("scale"))
        return TypeInfo(
            type_token=type_token,
            wire_type_name="d",
            value_class="decimal",
            cpp_type="std::int64_t",
            cpp_type_comment="fixed-point mantissa carried with generated scale metadata",
            storage_size_bytes=0,
            logical_length=0,
            decimal_digits=digits,
            decimal_scale=scale,
            sql_type=f"NUMERIC,{digits},{scale}",
            cgate_type="P2BCDII",
            description="Fixed-point decimal number coded in binary system.",
        )

    binary_match = BINARY_TYPE_RE.fullmatch(type_token)
    if binary_match:
        length = int(binary_match.group("length"))
        kind = binary_match.group("kind")
        return TypeInfo(
            type_token=type_token,
            wire_type_name="bN" if kind == "b" else "zN",
            value_class="binary",
            cpp_type="std::span<const std::byte>",
            cpp_type_comment="binary payload view",
            storage_size_bytes=length if kind == "b" else 0,
            logical_length=length,
            decimal_digits=0,
            decimal_scale=0,
            sql_type=f"VARBINARY,{length}",
            cgate_type="VARBINARY",
            description="Data unit.",
        )

    raise ValueError(f"unsupported PLAZA II type token: {type_token}")


def service_field_name(field_name: str) -> bool:
    return field_name in {"replID", "replRev", "replAct"}


def read_ini(path: Path) -> configparser.ConfigParser:
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str
    with path.open("r", encoding="utf-8") as handle:
        parser.read_file(handle)
    return parser


def _required_section(config: configparser.ConfigParser, section: str) -> configparser.SectionProxy:
    if not config.has_section(section):
        raise ValueError(f"missing required section [{section}]")
    return config[section]


def _required_key(section_name: str, section: configparser.SectionProxy, key: str) -> str:
    value = section.get(key)
    if value is None or not value.strip():
        raise ValueError(f"section [{section_name}] is missing required key {key}")
    return value.strip()


def parse_reviewed_scheme(schema_path: Path) -> dict[str, Any]:
    config = read_ini(schema_path)
    meta_section = _required_section(config, "meta")
    meta = {
        "version": int(_required_key("meta", meta_section, "version")),
        "schema_name": _required_key("meta", meta_section, "schema_name"),
        "source_artifact_id": _required_key("meta", meta_section, "source_artifact_id"),
        "source_relative_path": _required_key("meta", meta_section, "source_relative_path"),
        "source_sha256": _required_key("meta", meta_section, "source_sha256"),
        "streams_inventory_path": _required_key("meta", meta_section, "streams_inventory_path"),
        "tables_inventory_path": _required_key("meta", meta_section, "tables_inventory_path"),
    }

    stream_rows: list[dict[str, Any]] = []
    table_rows: list[dict[str, Any]] = []
    field_rows: list[dict[str, Any]] = []

    stream_names: set[str] = set()
    table_keys: set[tuple[str, str]] = set()
    field_keys: set[tuple[str, str, str]] = set()

    for section_name in config.sections():
        if section_name == "meta":
            continue
        match = SECTION_RE.fullmatch(section_name)
        if match is None:
            raise ValueError(f"unsupported section name [{section_name}]")
        section = config[section_name]
        kind = match.group("kind")

        if kind == "stream":
            stream_name = _required_key(section_name, section, "stream_name")
            if stream_name in stream_names:
                raise ValueError(f"duplicate stream section for {stream_name}")
            stream_names.add(stream_name)
            stream_rows.append(
                {
                    "order": int(_required_key(section_name, section, "order")),
                    "protocol_item_id": _required_key(section_name, section, "protocol_item_id"),
                    "stream_name": stream_name,
                    "stream_anchor_name": _required_key(section_name, section, "stream_anchor_name"),
                    "stream_type": _required_key(section_name, section, "stream_type"),
                    "title": _required_key(section_name, section, "title"),
                    "scope_bucket": _required_key(section_name, section, "scope_bucket"),
                    "scheme_filename": _required_key(section_name, section, "scheme_filename"),
                    "scheme_section": _required_key(section_name, section, "scheme_section"),
                    "matching_partitioned": parse_bool_strict(_required_key(section_name, section, "matching_partitioned")),
                    "login_subtypes": parse_csv_list(section.get("login_subtypes", "")),
                    "stream_variants": parse_csv_list(section.get("stream_variants", "")),
                    "default_variant": section.get("default_variant", "").strip(),
                }
            )
            continue

        if kind == "table":
            stream_name = _required_key(section_name, section, "stream_name")
            table_name = _required_key(section_name, section, "table_name")
            table_key = (stream_name, table_name)
            if table_key in table_keys:
                raise ValueError(f"duplicate table section for {stream_name}.{table_name}")
            table_keys.add(table_key)
            table_rows.append(
                {
                    "order": int(_required_key(section_name, section, "order")),
                    "protocol_item_id": _required_key(section_name, section, "protocol_item_id"),
                    "stream_name": stream_name,
                    "table_name": table_name,
                    "title": _required_key(section_name, section, "title"),
                    "scope_bucket": _required_key(section_name, section, "scope_bucket"),
                }
            )
            continue

        stream_name = _required_key(section_name, section, "stream_name")
        table_name = _required_key(section_name, section, "table_name")
        field_name = _required_key(section_name, section, "field_name")
        field_key = (stream_name, table_name, field_name)
        if field_key in field_keys:
            raise ValueError(f"duplicate field section for {stream_name}.{table_name}.{field_name}")
        field_keys.add(field_key)
        type_token = _required_key(section_name, section, "type_token")
        type_info = parse_type_info(type_token)
        field_rows.append(
            {
                "order": int(_required_key(section_name, section, "order")),
                "stream_name": stream_name,
                "table_name": table_name,
                "field_name": field_name,
                "type_token": type_token,
                "type_info": type_info,
                "description": _required_key(section_name, section, "description"),
                "service_field": service_field_name(field_name),
            }
        )

    stream_rows.sort(key=lambda row: (row["order"], row["stream_name"]))
    table_rows.sort(key=lambda row: (row["order"], row["stream_name"], row["table_name"]))
    field_rows.sort(key=lambda row: (row["order"], row["stream_name"], row["table_name"], row["field_name"]))

    if not stream_rows:
        raise ValueError("reviewed scheme is missing stream sections")
    if not table_rows:
        raise ValueError("reviewed scheme is missing table sections")
    if not field_rows:
        raise ValueError("reviewed scheme is missing field sections")

    stream_name_to_row = {row["stream_name"]: row for row in stream_rows}
    table_key_to_row = {(row["stream_name"], row["table_name"]): row for row in table_rows}

    for table in table_rows:
        if table["stream_name"] not in stream_name_to_row:
            raise ValueError(f"table references unknown stream {table['stream_name']}")

    for field in field_rows:
        table_key = (field["stream_name"], field["table_name"])
        if table_key not in table_key_to_row:
            raise ValueError(f"field references unknown table {field['stream_name']}.{field['table_name']}")

    stream_table_counts: dict[str, int] = {row["stream_name"]: 0 for row in stream_rows}
    table_field_counts: dict[tuple[str, str], int] = {key: 0 for key in table_key_to_row}
    for table in table_rows:
        stream_table_counts[table["stream_name"]] += 1
    for field in field_rows:
        table_field_counts[(field["stream_name"], field["table_name"])] += 1

    type_rows: list[dict[str, Any]] = []
    type_seen: set[str] = set()
    for field in field_rows:
        type_info = field["type_info"]
        if type_info.type_token in type_seen:
            continue
        type_seen.add(type_info.type_token)
        type_rows.append(
            {
                "type_token": type_info.type_token,
                "wire_type_name": type_info.wire_type_name,
                "value_class": type_info.value_class,
                "cpp_type": type_info.cpp_type,
                "cpp_type_comment": type_info.cpp_type_comment,
                "storage_size_bytes": type_info.storage_size_bytes,
                "logical_length": type_info.logical_length,
                "decimal_digits": type_info.decimal_digits,
                "decimal_scale": type_info.decimal_scale,
                "sql_type": type_info.sql_type,
                "cgate_type": type_info.cgate_type,
                "description": type_info.description,
                "type_id": stable_numeric_id("type", type_info.type_token),
                "protocol_item_id": stable_id("plaza2", "type", type_info.type_token),
            }
        )
    type_rows.sort(key=lambda row: row["type_token"])

    generated_streams: list[dict[str, Any]] = []
    generated_tables: list[dict[str, Any]] = []
    generated_fields: list[dict[str, Any]] = []

    table_index = 0
    field_index = 0
    for stream in stream_rows:
        stream_id = stable_numeric_id("stream", stream["stream_name"])
        stream_rows_for_stream = [
            table for table in table_rows
            if table["stream_name"] == stream["stream_name"]
        ]
        generated_streams.append(
            {
                **stream,
                "stream_id": stream_id,
                "table_count": stream_table_counts[stream["stream_name"]],
                "first_table_index": table_index,
            }
        )

        for table in stream_rows_for_stream:
            table_id = stable_numeric_id("table", table["stream_name"], table["table_name"])
            field_rows_for_table = [
                field for field in field_rows
                if field["stream_name"] == table["stream_name"] and field["table_name"] == table["table_name"]
            ]
            generated_tables.append(
                {
                    **table,
                    "stream_id": stream_id,
                    "table_id": table_id,
                    "field_count": table_field_counts[(table["stream_name"], table["table_name"])],
                    "first_field_index": field_index,
                }
            )
            for field in field_rows_for_table:
                type_info = field["type_info"]
                generated_fields.append(
                    {
                        "stream_name": field["stream_name"],
                        "table_name": field["table_name"],
                        "field_name": field["field_name"],
                        "field_id": stable_numeric_id("field", field["stream_name"], field["table_name"], field["field_name"]),
                        "stream_id": stream_id,
                        "table_id": table_id,
                        "table_order": table["order"],
                        "field_order": field["order"],
                        "type_token": field["type_token"],
                        "type_id": stable_numeric_id("type", field["type_token"]),
                        "value_class": type_info.value_class,
                        "cpp_type": type_info.cpp_type,
                        "cpp_type_comment": type_info.cpp_type_comment,
                        "storage_size_bytes": type_info.storage_size_bytes,
                        "logical_length": type_info.logical_length,
                        "decimal_digits": type_info.decimal_digits,
                        "decimal_scale": type_info.decimal_scale,
                        "description": field["description"],
                        "service_field": field["service_field"],
                    }
                )
                field_index += 1
            table_index += 1

    return {
        "schema": {
            **meta,
            "stream_count": len(generated_streams),
            "table_count": len(generated_tables),
            "field_count": len(generated_fields),
            "type_count": len(type_rows),
        },
        "types": type_rows,
        "streams": generated_streams,
        "tables": generated_tables,
        "fields": generated_fields,
    }
