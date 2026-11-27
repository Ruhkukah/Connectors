#!/usr/bin/env python3
"""Compare two local CGate vendor INIs or two directories of scheme INIs.

Report changes in table membership, wire field order and declarations, and
section attributes such as publisher message IDs and reply lists. Exit 0 for
identical declarations, 1 for differences, and 2 for invalid inputs.
"""

import argparse
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Section:
    fields: dict[str, str] = field(default_factory=dict)
    attributes: dict[str, list[str]] = field(default_factory=dict)


def read_scheme(path: Path) -> dict[str, Section]:
    data = path.read_bytes()
    try:
        content = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        content = data.decode("cp1251")
    sections = {}
    current = None
    for number, line in enumerate(content.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith((";", "#")):
            continue
        location = f"{path}:{number}"
        if line.startswith("["):
            if not line.endswith("]"):
                raise ValueError(f"{location}: invalid section header")
            name = line[1:-1]
            current = None
            if name.startswith(("dbscheme:", "table:")):
                if name in sections:
                    raise ValueError(f"{location}: duplicate section {name}")
                current = sections[name] = Section()
            continue
        if current is None:
            continue
        key, separator, value = line.partition("=")
        if not separator or not key.strip() or not value.strip():
            raise ValueError(f"{location}: invalid scheme declaration")
        key, value = key.strip(), value.strip()
        if key == "field":
            name, comma, declaration = value.partition(",")
            name, declaration = name.strip(), declaration.strip()
            if not comma or not name or not declaration or name in current.fields:
                raise ValueError(f"{location}: invalid or duplicate field {name}")
            current.fields[name] = declaration
        else:
            current.attributes.setdefault(key, []).append(value)
    return sections


def read_schemes(path: Path) -> dict[str, dict[str, Section]]:
    if path.is_file():
        sections = read_scheme(path)
        if sections:
            return {"": sections}
    elif path.is_dir():
        schemes = {}
        for ini in sorted(path.glob("*.ini")):
            sections = read_scheme(ini)
            if sections:
                schemes[ini.name] = sections
        if schemes:
            return schemes
    raise ValueError(f"{path}: no CGate scheme sections found")


def changes(before, after, label, compare):
    for name in sorted(before.keys() | after.keys()):
        key = f"{label} {name}" if label else name
        if name not in after:
            yield f"- {key}"
        elif name not in before:
            yield f"+ {key}"
        else:
            yield from compare(before[name], after[name], key)


def compare_section(before: Section, after: Section, label: str):
    for name in sorted(before.fields.keys() | after.fields.keys()):
        old = before.fields.get(name)
        new = after.fields.get(name)
        if old is None:
            yield f"+ {label}.{name} = {new}"
        elif new is None:
            yield f"- {label}.{name} = {old}"
        elif old != new:
            yield f"~ {label}.{name} = {old} -> {new}"
    common = before.fields.keys() & after.fields.keys()
    old_order = [name for name in before.fields if name in common]
    new_order = [name for name in after.fields if name in common]
    if old_order != new_order:
        yield f"~ {label} common field order = {', '.join(old_order)} -> {', '.join(new_order)}"
    for key in sorted(before.attributes.keys() | after.attributes.keys()):
        old, new = before.attributes.get(key, []), after.attributes.get(key, [])
        if old != new:
            yield f"~ {label} {key} = {old} -> {new}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    args = parser.parse_args()
    try:
        if args.before.is_file() != args.after.is_file():
            raise ValueError("provide two files or two directories")
        before, after = read_schemes(args.before), read_schemes(args.after)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    result = list(changes(before, after, "", lambda a, b, key: changes(a, b, key, compare_section)))
    if result:
        print("\n".join(result))
        return 1
    print("No scheme declaration differences.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
