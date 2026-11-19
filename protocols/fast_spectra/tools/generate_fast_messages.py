#!/usr/bin/env python3
"""Generate straight-line C++ decoders from a MOEX Spectra FAST templates.xml.

    generate_fast_messages.py <templates.xml> <output.hpp> [--check]

Only the template features the Spectra gate uses are accepted: mandatory
constants and operator-less fields. Anything else (copy, delta, default,
increment, optional constants, nested groups) would need presence-map and
dictionary handling the decoder deliberately omits, so generation fails
loudly instead of emitting a decoder that silently misreads the stream.
"""
import hashlib
import sys
import xml.etree.ElementTree as ET

NS = "{http://www.fixprotocol.org/ns/fast/td/1.1}"
OPERATORS = {"copy", "delta", "default", "increment", "tail"}
# xml type -> (C++ type, mandatory reader, optional reader)
SCALARS = {
    "uInt32": ("std::uint32_t", "read_u32", "read_opt_u32"),
    "int32": ("std::int32_t", "read_i32", "read_opt_i32"),
    "uInt64": ("std::uint64_t", "read_u64", "read_opt_u64"),
    "int64": ("std::int64_t", "read_i64", "read_opt_i64"),
    "decimal": ("Decimal", "read_decimal", "read_opt_decimal"),
    "string": ("Ascii", "read_ascii", "read_opt_ascii"),
    "unicode": ("std::string_view", "read_bytes", "read_opt_bytes"),
}


def fail(message):
    sys.exit(f"generate_fast_messages: {message}")


def local(element):
    return element.tag.removeprefix(NS)


def cpp_literal(kind, value):
    if kind == "string":
        return "std::string_view", '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return SCALARS[kind][0], value


class Struct:
    def __init__(self, name):
        self.name = name
        self.constants = []  # (type, name, literal, tag)
        self.members = []  # (type, name, tag)
        self.nested = []  # Struct
        self.steps = []  # ("field", name, reader) | ("sequence", Struct, count_name, optional)


def build(element, name, where, allow_sequences):
    out = Struct(name)
    for child in element:
        kind, field = local(child), child.get("name")
        path = f"{where}.{field}"
        optional = child.get("presence") == "optional"
        if kind == "sequence":
            if not allow_sequences:
                fail(f"{path}: nested sequences are not supported")
            length = child.find(NS + "length")
            if length is None or len(length):
                fail(f"{path}: sequence needs a plain <length>")
            entry = build([c for c in child if local(c) != "length"], field, path, False)
            count = length.get("name")
            count_type = "Opt<std::uint32_t>" if optional else "std::uint32_t"
            out.members.append((count_type, count, length.get("id")))
            out.nested.append(entry)
            out.steps.append(("sequence", entry, count, optional))
            continue
        if kind not in SCALARS:
            fail(f"{path}: unsupported element <{kind}>")
        operators = [local(c) for c in child]
        if any(op in OPERATORS for op in operators):
            fail(f"{path}: operator {operators} is not supported")
        if operators == ["constant"]:
            if optional:
                fail(f"{path}: optional constants need a presence-map bit")
            out.constants.append((*cpp_literal(kind, child[0].get("value")), field, child.get("id")))
            continue
        if operators:
            fail(f"{path}: unexpected children {operators}")
        if kind == "string" and child.get("charset") == "unicode":
            kind = "unicode"
        cpp_type, reader, optional_reader = SCALARS[kind]
        if optional:
            cpp_type, reader = f"Opt<{cpp_type}>", optional_reader
        out.members.append((cpp_type, field, child.get("id")))
        out.steps.append(("field", field, reader))
    return out


def emit_struct(struct, indent, template_id=None):
    pad = " " * indent
    lines = [f"{pad}struct {struct.name} {{"]
    if template_id is not None:
        lines.append(f"{pad}    static constexpr std::uint32_t kTemplateId = {template_id};")
    for cpp_type, literal, name, tag in struct.constants:
        lines.append(f"{pad}    static constexpr {cpp_type} {name} = {literal}; // {tag}")
    for nested in struct.nested:
        lines += emit_struct(nested, indent + 4)
    for cpp_type, name, tag in struct.members:
        lines.append(f"{pad}    {cpp_type} {name}; // {tag}")
    lines.append(f"{pad}}};")
    return lines


def emit_decoder(struct):
    has_sequences = bool(struct.nested)
    lines = []
    # Entries are self-delimiting, so each group gets a standalone decoder; a
    # consumer can keep an entry's bytes (Reader::mark .. Reader::p) and decode them later.
    for entry in struct.nested:
        lines.append(f"MOEX_FAST_INLINE bool decode(Reader& r, {struct.name}::{entry.name}& e) noexcept {{")
        for _, name, reader in entry.steps:
            lines.append(f"    e.{name} = {reader}(r);")
        lines.append("    return r.ok;")
        lines.append("}")
        lines.append("")
    if has_sequences:
        lines.append("template <class Handler>")
        lines.append(f"inline bool decode(Reader& r, {struct.name}& m, Handler&& on_entry) noexcept {{")
    else:
        lines.append(f"inline bool decode(Reader& r, {struct.name}& m) noexcept {{")
    for step in struct.steps:
        if step[0] == "field":
            lines.append(f"    m.{step[1]} = {step[2]}(r);")
            continue
        _, entry, count, optional = step
        if optional:
            lines.append(f"    m.{count} = read_opt_length(r);")
            bound = f"m.{count}.value"
        else:
            lines.append(f"    m.{count} = read_length(r);")
            bound = f"m.{count}"
        lines.append(f"    for (std::uint32_t i = 0; i < {bound}; ++i) {{")
        lines.append(f"        {struct.name}::{entry.name} e;")
        lines.append("        r.mark = r.p;")
        lines.append("        if (!decode(r, e))")
        lines.append("            return false;")
        lines.append("        on_entry(std::as_const(m), std::as_const(e));")
        lines.append("    }")
    lines.append("    return r.ok;")
    lines.append("}")
    return lines


def generate(source_bytes):
    root = ET.fromstring(source_bytes)
    templates = []
    for template in root.findall(NS + "template"):
        name, template_id = template.get("name"), int(template.get("id"))
        templates.append((template_id, build(template, name, name, True)))
    if not templates:
        fail("no templates found")
    digest = hashlib.sha256(source_bytes).hexdigest()
    lines = [
        "// Generated by protocols/fast_spectra/tools/generate_fast_messages.py. Do not edit.",
        "#pragma once",
        "",
        '#include "moex/fast/fast_wire.hpp"',
        "",
        "#include <cstdint>",
        "#include <string_view>",
        "#include <utility>",
        "",
        "// Entry decoders are large; left to its own judgement the compiler stops inlining them",
        "// into the message loop and the hot path slows down about threefold.",
        "#if defined(__GNUC__) || defined(__clang__)",
        "#define MOEX_FAST_INLINE [[gnu::always_inline]] inline",
        "#else",
        "#define MOEX_FAST_INLINE inline",
        "#endif",
        "",
        "namespace moex::fast {",
        "",
        "// SHA-256 of the templates.xml this decoder was generated from.",
        f'inline constexpr std::string_view kTemplatesSha256 = "{digest}";',
        "",
    ]
    for template_id, struct in templates:
        lines += emit_struct(struct, 0, template_id)
        lines.append("")
        lines += emit_decoder(struct)
        lines.append("")
    lines.append("[[nodiscard]] constexpr std::string_view template_name(std::uint32_t id) noexcept {")
    lines.append("    switch (id) {")
    for template_id, struct in sorted(templates, key=lambda item: item[0]):
        lines.append(f"    case {template_id}:")
        lines.append(f'        return "{struct.name}";')
    lines.append("    default:")
    lines.append("        return {};")
    lines.append("    }")
    lines.append("}")
    lines.append("")
    lines.append("} // namespace moex::fast")
    return "\n".join(lines) + "\n"


def main(argv):
    check = "--check" in argv
    paths = [arg for arg in argv[1:] if arg != "--check"]
    if len(paths) != 2:
        fail(__doc__.strip().splitlines()[2].strip())
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
