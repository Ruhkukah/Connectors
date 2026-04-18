#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import textwrap

from moex_phase0_common import ensure_dir
from twime_schema_common import cpp_string, parse_twime_schema


ENUM_PRIMITIVE_CPP = {
    "char": "char",
    "uint8": "std::uint8_t",
    "uint16": "std::uint16_t",
    "uint32": "std::uint32_t",
    "uint64": "std::uint64_t",
    "int8": "std::int8_t",
    "int16": "std::int16_t",
    "int32": "std::int32_t",
    "int64": "std::int64_t",
}

PRIMITIVE_ENUM_LABEL = {
    "int8": "Int8",
    "int16": "Int16",
    "int32": "Int32",
    "int64": "Int64",
    "uint8": "UInt8",
    "uint16": "UInt16",
    "uint32": "UInt32",
    "uint64": "UInt64",
    "char": "Char",
    "composite": "None",
}

FIELD_KIND_LABEL = {
    "primitive": "Primitive",
    "string": "String",
    "delta_millisecs": "DeltaMillisecs",
    "timestamp": "TimeStamp",
    "enum": "Enum",
    "set": "Set",
    "decimal5": "Decimal5",
    "composite": "Composite",
}


def _cpp_u64(value: int) -> str:
    if value == 18446744073709551615:
        return "std::numeric_limits<std::uint64_t>::max()"
    if value > 9223372036854775807:
        return f"{value}ULL"
    return str(value)


def _generated_enums(parsed: dict) -> str:
    enums = []
    sets = []
    for item in parsed["enums"]:
        if item["protocol_item_id"].startswith("twime_enum_"):
            base_type = ENUM_PRIMITIVE_CPP[item["encoding_type"]]
            lines = [f"enum class {item['enum_name']} : {base_type} {{"]
            for value in item["values"]:
                lines.append(f"    {value['name']} = {value['wire_value']},")
            lines.append("};")
            enums.append("\n".join(lines))
        else:
            base_type = ENUM_PRIMITIVE_CPP[item["encoding_type"]]
            lines = [f"namespace {item['enum_name']}Mask {{"]
            for value in item["values"]:
                bit_index = int(value["bit_index"])
                lines.append(f"inline constexpr {base_type} {value['name']} = static_cast<{base_type}>(1u << {bit_index});")
            lines.append("}  // namespace")
            sets.append("\n".join(lines))

    body = "\n\n".join(enums + sets)
    return textwrap.dedent(
        f"""\
        #pragma once

        #include <cstdint>

        namespace moex::twime_sbe {{

        {body}

        }}  // namespace moex::twime_sbe
        """
    )


def _generated_messages(parsed: dict) -> str:
    entries = "\n".join(
        f"    {item['message_name']} = {item['template_id']}," for item in parsed["messages"]
    )
    return textwrap.dedent(
        f"""\
        #pragma once

        #include <cstdint>

        namespace moex::twime_sbe {{

        enum class TwimeTemplateId : std::uint16_t {{
        {entries}
        }};

        }}  // namespace moex::twime_sbe
        """
    )


def _generated_metadata_hpp() -> str:
    return textwrap.dedent(
        """\
        #pragma once

        #include "moex/twime_sbe/twime_schema.hpp"

        namespace moex::twime_sbe::generated {

        const TwimeSchemaInfo& schema_info() noexcept;
        std::span<const TwimeTypeMetadata> types() noexcept;
        std::span<const TwimeEnumMetadata> enums() noexcept;
        std::span<const TwimeSetMetadata> sets() noexcept;
        std::span<const TwimeMessageMetadata> messages() noexcept;
        const TwimeTypeMetadata* find_type(std::string_view name) noexcept;
        const TwimeEnumMetadata* find_enum(std::string_view name) noexcept;
        const TwimeSetMetadata* find_set(std::string_view name) noexcept;
        const TwimeMessageMetadata* find_message_by_name(std::string_view name) noexcept;
        const TwimeMessageMetadata* find_message_by_template_id(std::uint16_t template_id) noexcept;

        }  // namespace moex::twime_sbe::generated
        """
    )


def _emit_lookup(name: str, container: str, key_accessor: str, return_expr: str) -> str:
    return textwrap.dedent(
        f"""\
        const {return_expr}* {name}(std::string_view key) noexcept {{
            for (const auto& item : {container}) {{
                if ({key_accessor} == key) {{
                    return &item;
                }}
            }}
            return nullptr;
        }}
        """
    )


def _generated_metadata_cpp(parsed: dict) -> str:
    schema = parsed["schema"]
    types_by_name = parsed["types_by_name"]
    type_names = list(types_by_name.keys())
    enum_names = [item["enum_name"] for item in parsed["enums"] if item["protocol_item_id"].startswith("twime_enum_")]
    set_names = [item["enum_name"] for item in parsed["enums"] if item["protocol_item_id"].startswith("twime_set_")]

    lines: list[str] = [
        '#include "moex/twime_sbe/twime_schema.hpp"',
        '#include "twime_generated_metadata.hpp"',
        "",
        "#include <array>",
        "#include <limits>",
        "",
        'using namespace std::literals;',
        "",
        "namespace moex::twime_sbe::generated {",
        "namespace {",
        "",
    ]

    for item in parsed["enums"]:
        if item["protocol_item_id"].startswith("twime_enum_"):
            array_name = f"kEnumValues_{item['enum_name']}"
            lines.append(
                f"constexpr std::array<TwimeEnumValueMetadata, {len(item['values'])}> {array_name}{{{{"
            )
            for value in item["values"]:
                lines.append(
                    f'    {{{cpp_string(value["name"])}, {value["wire_value"]}, {cpp_string(value["wire_text"])}}},'
                )
            lines.append("}};")
            lines.append(
                f"constexpr TwimeEnumMetadata kEnum_{item['enum_name']}{{"
                f"{cpp_string(item['enum_name'])}, "
                f"TwimePrimitiveType::{PRIMITIVE_ENUM_LABEL[item['encoding_type']]}, "
                f"{item['encoded_size']}, "
                f"{array_name}.data(), "
                f"{array_name}.size()"
                "};"
            )
            lines.append("")
        else:
            array_name = f"kSetChoices_{item['enum_name']}"
            lines.append(
                f"constexpr std::array<TwimeSetChoiceMetadata, {len(item['values'])}> {array_name}{{{{"
            )
            for value in item["values"]:
                lines.append(
                    f'    {{{cpp_string(value["name"])}, {int(value["bit_index"])}, {cpp_string(value.get("description", ""))}}},'
                )
            lines.append("}};")
            lines.append(
                f"constexpr TwimeSetMetadata kSet_{item['enum_name']}{{"
                f"{cpp_string(item['enum_name'])}, "
                f"TwimePrimitiveType::{PRIMITIVE_ENUM_LABEL[item['encoding_type']]}, "
                f"{item['encoded_size']}, "
                f"{array_name}.data(), "
                f"{array_name}.size()"
                "};"
            )
            lines.append("")

    lines.append(f"constexpr std::array<TwimeTypeMetadata, {len(type_names)}> kTypes{{{{")
    for type_name in type_names:
        item = types_by_name[type_name]
        enum_ptr = f"&kEnum_{type_name}" if item["kind"] == "enum" else "nullptr"
        set_ptr = f"&kSet_{type_name}" if item["kind"] == "set" else "nullptr"
        null_value = item["null_value"] if item["null_value"] is not None else 0
        has_null = "true" if item["null_value"] is not None else "false"
        lines.append(
            "    {"
            f"{cpp_string(type_name)}, "
            f"TwimeFieldKind::{FIELD_KIND_LABEL[item['kind']]}, "
            f"TwimePrimitiveType::{PRIMITIVE_ENUM_LABEL[item['primitive_type']]}, "
            f"{item['encoded_size']}, "
            f"{'true' if item.get('presence') == 'optional' or item.get('null_value') is not None else 'false'}, "
            f"static_cast<std::uint64_t>({_cpp_u64(null_value)}), "
            f"{has_null}, "
            f"{item.get('fixed_length', 0)}, "
            f"{item.get('constant_exponent', 0) if item['kind'] == 'decimal5' else 0}, "
            f"{enum_ptr}, "
            f"{set_ptr}, "
            f"{cpp_string(item.get('description', ''))}"
            "},"
        )
    lines.append("}};")
    lines.append("")

    for message in parsed["messages"]:
        field_array_name = f"kFields_{message['message_name']}"
        lines.append(
            f"constexpr std::array<TwimeFieldMetadata, {len(message['fields'])}> {field_array_name}{{{{"
        )
        for field in message["fields"]:
            type_name = field["type"]
            lines.append(
                "    {"
                f"{cpp_string(field['name'])}, "
                f"{field['tag']}, "
                f"&kTypes[{type_names.index(type_name)}], "
                f"{field['encoded_size']}, "
                f"{field['offset']}, "
                f"{'true' if field['nullable'] else 'false'}, "
                f"static_cast<std::uint64_t>({_cpp_u64(field['null_value'] if field['null_value'] is not None else 0)}), "
                f"{'true' if field['null_value'] is not None else 'false'}"
                "},"
            )
        lines.append("}};")
        direction_label = {
            "client_to_server": "ClientToServer",
            "server_to_client": "ServerToClient",
            "bidirectional": "Bidirectional",
            "unknown": "Unknown",
        }[message["direction"]]
        layer_label = {"session": "Session", "application": "Application"}[message["layer"]]
        lines.append(
            f"constexpr TwimeMessageMetadata kMessage_{message['message_name']}{{"
            f"{cpp_string(message['message_name'])}, "
            f"{message['template_id']}, "
            f"TwimeLayer::{layer_label}, "
            f"TwimeDirection::{direction_label}, "
            f"{message['block_length']}, "
            f"{field_array_name}.data(), "
            f"{field_array_name}.size()"
            "};"
        )
        lines.append("")

    lines.append(f"constexpr std::array<TwimeMessageMetadata, {len(parsed['messages'])}> kMessages{{{{")
    for message in parsed["messages"]:
        lines.append(f"    kMessage_{message['message_name']},")
    lines.append("}};")
    lines.append("")
    lines.append(f"constexpr std::array<TwimeEnumMetadata, {len(enum_names)}> kAllEnums{{{{")
    for name in enum_names:
        lines.append(f"    kEnum_{name},")
    lines.append("}};")
    lines.append("")
    lines.append(f"constexpr std::array<TwimeSetMetadata, {len(set_names)}> kAllSets{{{{")
    for name in set_names:
        lines.append(f"    kSet_{name},")
    lines.append("}};")
    lines.append("")
    lines.append(
        "constexpr TwimeSchemaInfo kSchemaInfo{"
        f"{cpp_string(schema['package'])}, "
        f"{schema['schema_id']}, "
        f"{schema['schema_version']}, "
        f"{cpp_string(schema['byte_order'])}, "
        f"{cpp_string(schema['sha256'])}"
        "};"
    )
    lines.extend(
        [
            "",
            "}  // namespace",
            "",
            "const TwimeSchemaInfo& schema_info() noexcept { return kSchemaInfo; }",
            "std::span<const TwimeTypeMetadata> types() noexcept { return kTypes; }",
            "std::span<const TwimeEnumMetadata> enums() noexcept { return kAllEnums; }",
            "std::span<const TwimeSetMetadata> sets() noexcept { return kAllSets; }",
            "std::span<const TwimeMessageMetadata> messages() noexcept { return kMessages; }",
            _emit_lookup("find_type", "kTypes", "item.name", "TwimeTypeMetadata").strip(),
            "",
            "const TwimeEnumMetadata* find_enum(std::string_view key) noexcept {",
            "    for (const auto& item : enums()) {",
            "        if (item.name == key) {",
            "            return &item;",
            "        }",
            "    }",
            "    return nullptr;",
            "}",
            "",
            "const TwimeSetMetadata* find_set(std::string_view key) noexcept {",
            "    for (const auto& item : sets()) {",
            "        if (item.name == key) {",
            "            return &item;",
            "        }",
            "    }",
            "    return nullptr;",
            "}",
            "",
            _emit_lookup("find_message_by_name", "kMessages", "item.name", "TwimeMessageMetadata").strip(),
            "",
            "const TwimeMessageMetadata* find_message_by_template_id(std::uint16_t template_id) noexcept {",
            "    for (const auto& item : kMessages) {",
            "        if (item.template_id == template_id) {",
            "            return &item;",
            "        }",
            "    }",
            "    return nullptr;",
            "}",
            "",
            "}  // namespace moex::twime_sbe::generated",
        ]
    )
    return "\n".join(lines) + "\n"


def _write_or_check(path: Path, content: str, check: bool) -> None:
    if check:
        existing = path.read_text(encoding="utf-8")
        if existing != content:
            raise ValueError(f"Generated file is out of date: {path}")
        return
    path.write_text(content, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate deterministic TWIME metadata C++ from the pinned schema.")
    parser.add_argument("--schema", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    schema_path = Path(args.schema).resolve()
    out_dir = ensure_dir(Path(args.out).resolve())
    parsed = parse_twime_schema(schema_path)

    outputs = {
        out_dir / "twime_generated_enums.hpp": _generated_enums(parsed),
        out_dir / "twime_generated_messages.hpp": _generated_messages(parsed),
        out_dir / "twime_generated_metadata.hpp": _generated_metadata_hpp(),
        out_dir / "twime_generated_metadata.cpp": _generated_metadata_cpp(parsed),
    }

    for path, content in outputs.items():
        _write_or_check(path, content, args.check)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
