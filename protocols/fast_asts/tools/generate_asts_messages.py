#!/usr/bin/env python3
"""Generate C++ decoders from a MOEX ASTS FAST templates file (FIX50SP2-*.xml).

    generate_asts_messages.py <templates.xml> <output.hpp> [--check]

Supports what the ASTS gate uses: mandatory constants, copy on optional fields,
default with a template value, increment, optional sequences and byte vectors.
The gate resets its dictionary after every packet and sends one message per
packet, so "previous value" state lives only inside one message: a copy field
in a sequence is simply left untouched in the entry struct when its presence
bit is clear. Anything else (delta, tail, initial values on copy, operators the
generator has not been taught) fails generation rather than decoding wrongly.
"""
import hashlib
import re
import sys
import xml.etree.ElementTree as ET

NS = "{http://www.fixprotocol.org/ns/fast/td/1.1}"
# xml type -> (C++ type, mandatory reader, optional reader)
SCALARS = {
    "uInt32": ("std::uint32_t", "read_u32", "read_opt_u32"),
    "int32": ("std::int32_t", "read_i32", "read_opt_i32"),
    "uInt64": ("std::uint64_t", "read_u64", "read_opt_u64"),
    "int64": ("std::int64_t", "read_i64", "read_opt_i64"),
    "decimal": ("Decimal", "read_decimal", "read_opt_decimal"),
    "string": ("Ascii", "read_ascii", "read_opt_ascii"),
    "byteVector": ("std::string_view", "read_bytes", "read_opt_bytes"),
}
# FIX message types used as template names.
NAMES = {"A": "Logon", "5": "Logout", "0": "Heartbeat", "d": "SecurityDefinition", "f": "SecurityStatus",
         "h": "TradingSessionStatus"}


def fail(message):
    sys.exit(f"generate_asts_messages: {message}")


def local(element):
    return element.tag.removeprefix(NS)


def identifier(name):
    return NAMES.get(name, re.sub(r"\W", "_", name))


def quoted(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


class Struct:
    def __init__(self, name):
        self.name = name
        self.constants = []  # (type, name, literal, tag)
        self.members = []  # (type, name, tag)
        self.nested = []  # Struct
        self.steps = []  # ("field", name, reader, operator, extra) | ("sequence", Struct, count, optional)
        self.statics = []  # C++ lines declared before the struct

    def uses_pmap(self):
        return any(step[0] == "field" and step[3] for step in self.steps)


def build(elements, name, where, top):
    out = Struct(name)
    for child in elements:
        kind, field = local(child), child.get("name")
        path = f"{where}.{field}"
        optional = child.get("presence") == "optional"
        for attribute in ("dictionary", "key", "charset"):
            if child.get(attribute):
                fail(f"{path}: attribute {attribute} is not supported")
        if kind == "sequence":
            length = child.find(NS + "length")
            if length is None or len(length):
                fail(f"{path}: sequence needs a plain <length>")
            entry = build([c for c in child if local(c) != "length"], field, path, False)
            count = length.get("name")
            out.members.append(("Opt<std::uint32_t>" if optional else "std::uint32_t", count, length.get("id")))
            out.nested.append(entry)
            out.steps.append(("sequence", entry, count, optional))
            continue
        if kind not in SCALARS:
            fail(f"{path}: unsupported element <{kind}>")
        cpp_type, reader, optional_reader = SCALARS[kind]
        operators = [(local(c), c.get("value")) for c in child]
        if len(operators) > 1:
            fail(f"{path}: more than one operator")
        operator, value = operators[0] if operators else (None, None)
        if operator == "constant":
            if optional or kind != "string":
                fail(f"{path}: only mandatory string constants are supported")
            out.constants.append(("std::string_view", field, quoted(value), child.get("id")))
            continue
        extra = None
        if operator == "copy":
            # Optional, no initial value: an unset bit with nothing before it means absent.
            if not optional or value is not None:
                fail(f"{path}: copy is supported on optional fields without an initial value")
        elif operator == "default":
            if optional or kind != "string" or value is None:
                fail(f"{path}: default is supported on mandatory strings with a value")
            extra = f"k{name}_{field}_default"
            encoded = ", ".join(f"0x{byte:02X}" for byte in value.encode("ascii"))
            out.statics.append(f"inline constexpr std::uint8_t {extra}[] = {{{encoded}}};")
        elif operator == "increment":
            # With the dictionary reset per packet there is never a previous value to add to.
            if optional or value is not None or not top:
                fail(f"{path}: increment is supported on mandatory top-level fields without an initial value")
        elif operator is not None:
            fail(f"{path}: operator <{operator}> is not supported")
        if optional:
            cpp_type, reader = f"Opt<{cpp_type}>", optional_reader
        out.members.append((cpp_type, field, child.get("id")))
        out.steps.append(("field", field, reader, operator, extra))
    return out


def emit_struct(struct, indent, template_id=None):
    pad = " " * indent
    lines = [f"{pad}struct {struct.name} {{"]
    if template_id is not None:
        lines.append(f"{pad}    static constexpr std::uint32_t kTemplateId = {template_id};")
    for cpp_type, name, literal, tag in struct.constants:
        lines.append(f"{pad}    static constexpr {cpp_type} {name} = {literal}; // {tag}")
    for nested in struct.nested:
        lines += emit_struct(nested, indent + 4)
    for cpp_type, name, tag in struct.members:
        lines.append(f"{pad}    {cpp_type} {name}; // {tag}")
    lines.append(f"{pad}}};")
    return lines


def emit_fields(struct, target, pmap, pad):
    lines = []
    for step in struct.steps:
        if step[0] != "field":
            continue
        _, name, reader, operator, extra = step
        read = f"{target}.{name} = {reader}(r);"
        if operator is None:
            lines.append(f"{pad}{read}")
        elif operator == "copy":
            lines.append(f"{pad}if (take({pmap}))")
            lines.append(f"{pad}    {read}")
        elif operator == "default":
            lines.append(f"{pad}{target}.{name} = take({pmap}) ? {reader}(r) : literal({extra});")
        else:  # increment
            lines.append(f"{pad}if (take({pmap}))")
            lines.append(f"{pad}    {read}")
            lines.append(f"{pad}else")
            lines.append(f"{pad}    r.fail();")
    return lines


def all_statics(struct):
    lines = list(struct.statics)
    for nested in struct.nested:
        lines += all_statics(nested)
    return lines


def emit_body(struct, type_path, target, pmap, pad, depth):
    """Decode statements for one struct; sequences recurse with their own entry object and presence map."""
    lines = []
    for step in struct.steps:
        if step[0] == "field":
            single = Struct(struct.name)
            single.steps = [step]
            lines += emit_fields(single, target, pmap, pad)
            continue
        _, entry, count, optional = step
        if optional:
            lines.append(f"{pad}{target}.{count} = read_opt_length(r);")
            bound = f"{target}.{count}.value"
        else:
            lines.append(f"{pad}{target}.{count} = read_length(r);")
            bound = f"{target}.{count}"
        suffix = "" if depth == 0 else str(depth + 1)
        entry_type = f"{type_path}::{entry.name}"
        lines.append(f"{pad}{{")
        # One entry object for the whole sequence: a clear presence bit leaves a copy field as it was.
        lines.append(f"{pad}    {entry_type} e{suffix}{{}};")
        lines.append(f"{pad}    for (std::uint32_t i{suffix} = 0; i{suffix} < {bound}; ++i{suffix}) {{")
        if entry.uses_pmap():
            lines.append(f"{pad}        std::uint64_t bits{suffix} = read_pmap(r);")
        lines += emit_body(entry, entry_type, f"e{suffix}", f"bits{suffix}", pad + "        ", depth + 1)
        lines.append(f"{pad}        if (!r.ok)")
        lines.append(f"{pad}            return false;")
        lines.append(f"{pad}        on_entry(std::as_const(m), std::as_const(e{suffix}));")
        lines.append(f"{pad}    }}")
        lines.append(f"{pad}}}")
    return lines


def emit_decoder(struct):
    lines = all_statics(struct)
    signature = f"Reader& r, std::uint64_t pmap, {struct.name}& m"
    if struct.nested:
        lines.append("template <class Handler>")
        lines.append(f"inline bool decode({signature}, Handler&& on_entry) noexcept {{")
    else:
        lines.append(f"inline bool decode({signature}) noexcept {{")
    if not struct.uses_pmap():
        lines.append("    (void)pmap;")
    lines += emit_body(struct, struct.name, "m", "pmap", "    ", 0)
    lines.append("    return r.ok;")
    lines.append("}")
    return lines


def generate(source_bytes):
    root = ET.fromstring(source_bytes)
    templates = []
    for template in root.iter(NS + "template"):
        name = identifier(template.get("name"))
        templates.append((int(template.get("id")), template.get("name"), build(template, name, name, True)))
    if not templates:
        fail("no templates found")
    lines = [
        "// Generated by protocols/fast_asts/tools/generate_asts_messages.py. Do not edit.",
        "#pragma once",
        "",
        '#include "moex/fast/asts/asts_wire.hpp"',
        "",
        "#include <cstdint>",
        "#include <string_view>",
        "#include <utility>",
        "",
        "namespace moex::fast::asts {",
        "",
        "// SHA-256 of the templates file this decoder was generated from.",
        f'inline constexpr std::string_view kTemplatesSha256 = "{hashlib.sha256(source_bytes).hexdigest()}";',
        "",
    ]
    for template_id, _, struct in templates:
        lines += emit_struct(struct, 0, template_id)
        lines.append("")
        lines += emit_decoder(struct)
        lines.append("")
    lines.append("// Template name as written in the templates file.")
    lines.append("[[nodiscard]] constexpr std::string_view template_name(std::uint32_t id) noexcept {")
    lines.append("    switch (id) {")
    for template_id, original, _ in sorted(templates, key=lambda item: item[0]):
        lines.append(f"    case {template_id}:")
        lines.append(f"        return {quoted(original)};")
    lines.append("    default:")
    lines.append("        return {};")
    lines.append("    }")
    lines.append("}")
    lines.append("")
    lines.append("} // namespace moex::fast::asts")
    return "\n".join(lines) + "\n"


def main(argv):
    check = "--check" in argv
    paths = [arg for arg in argv[1:] if arg != "--check"]
    if len(paths) != 2:
        fail("usage: generate_asts_messages.py <templates.xml> <output.hpp> [--check]")
    with open(paths[0], "rb") as source:
        text = generate(source.read())
    if check:
        with open(paths[1], encoding="utf-8") as existing:
            if existing.read() != text:
                fail(f"{paths[1]} is stale; regenerate it")
        return
    with open(paths[1], "w", encoding="utf-8") as output:
        output.write(text)


if __name__ == "__main__":
    main(sys.argv)
