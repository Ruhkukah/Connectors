#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
import sys
import unicodedata
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class AllowlistedLocation:
    path: str
    line: int
    column: int
    code_point: int
    reason: str


ALLOWLIST: tuple[AllowlistedLocation, ...] = ()

EXPLICITLY_BLOCKED = set(range(0x202A, 0x202F)) | set(range(0x2066, 0x206A)) | {0x200E, 0x200F}
ALLOWED_CONTROL_WHITESPACE = {"\n", "\r", "\t"}


def tracked_files(paths: list[str], root: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--", *paths],
        cwd=root,
        capture_output=True,
        text=True,
        check=True,
    )
    return [root / item for item in result.stdout.splitlines() if item]


def is_allowlisted(path: Path, line: int, column: int, code_point: int, root: Path) -> bool:
    relative = path.relative_to(root).as_posix()
    for item in ALLOWLIST:
        if (
            item.path == relative
            and item.line == line
            and item.column == column
            and item.code_point == code_point
        ):
            return True
    return False


def scan_text(path: Path, root: Path) -> list[str]:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        return [f"{path.relative_to(root)}: decode error: {error}"]

    failures: list[str] = []
    for index, character in enumerate(text):
        code_point = ord(character)
        category = unicodedata.category(character)
        blocked = code_point in EXPLICITLY_BLOCKED or (
            category in {"Cf", "Cc"} and character not in ALLOWED_CONTROL_WHITESPACE
        )
        if not blocked:
            continue
        line = text.count("\n", 0, index) + 1
        column = index - text.rfind("\n", 0, index)
        if is_allowlisted(path, line, column, code_point, root):
            continue
        name = unicodedata.name(character, "UNKNOWN")
        failures.append(
            f"{path.relative_to(root)}:{line}:{column}: disallowed hidden Unicode {name} (U+{code_point:04X})"
        )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description="Fail on hidden Unicode controls in tracked source files.")
    parser.add_argument("paths", nargs="+", help="Repo-relative paths to scan via git ls-files.")
    args = parser.parse_args()

    root = Path.cwd().resolve()
    failures: list[str] = []
    seen: set[Path] = set()
    for path in tracked_files(args.paths, root):
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        failures.extend(scan_text(path, root))

    if failures:
        print("\n".join(failures))
        return 1

    print("unicode guard passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
