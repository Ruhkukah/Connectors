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
SUSPICIOUS_INVISIBLE_SPACES = {0x00A0, 0x2007, 0x202F}


def tracked_files(paths: list[str], root: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--", *paths],
        cwd=root,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return []
    return [root / item for item in result.stdout.splitlines() if item]


def resolve_scan_targets(paths: list[str], root: Path) -> list[Path]:
    discovered: list[Path] = []
    seen: set[Path] = set()

    for raw_path in paths:
        candidate = Path(raw_path)
        if candidate.exists():
            resolved = candidate.resolve()
            if resolved.is_file():
                if resolved not in seen:
                    discovered.append(resolved)
                    seen.add(resolved)
                continue
            if resolved.is_dir():
                for tracked in tracked_files([str(resolved.relative_to(root))], root):
                    if tracked.is_file() and tracked not in seen:
                        discovered.append(tracked)
                        seen.add(tracked)
                if not any(path == resolved or resolved in path.parents for path in discovered):
                    for nested in sorted(resolved.rglob("*")):
                        if nested.is_file():
                            nested = nested.resolve()
                            if nested not in seen:
                                discovered.append(nested)
                                seen.add(nested)
                continue

        for tracked in tracked_files([raw_path], root):
            tracked = tracked.resolve()
            if tracked.is_file() and tracked not in seen:
                discovered.append(tracked)
                seen.add(tracked)

    return discovered


def display_path(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def is_allowlisted(path: Path, line: int, column: int, code_point: int, root: Path) -> bool:
    relative = display_path(path, root)
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
        return [f"{display_path(path, root)}: decode error: {error}"]

    failures: list[str] = []
    for index, character in enumerate(text):
        code_point = ord(character)
        category = unicodedata.category(character)
        blocked = (
            code_point in EXPLICITLY_BLOCKED
            or code_point in SUSPICIOUS_INVISIBLE_SPACES
            or (category in {"Cf", "Cc"} and character not in ALLOWED_CONTROL_WHITESPACE)
        )
        if not blocked:
            continue
        line = text.count("\n", 0, index) + 1
        column = index - text.rfind("\n", 0, index)
        if is_allowlisted(path, line, column, code_point, root):
            continue
        name = unicodedata.name(character, "UNKNOWN")
        failures.append(
            f"{display_path(path, root)}:{line}:{column}: disallowed hidden Unicode {name} (U+{code_point:04X})"
        )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description="Fail on hidden Unicode controls in source or diff files.")
    parser.add_argument("paths", nargs="+", help="Repo-relative tracked paths or explicit files/directories to scan.")
    args = parser.parse_args()

    root = Path.cwd().resolve()
    failures: list[str] = []
    for path in resolve_scan_targets(args.paths, root):
        if not path.is_file():
            continue
        failures.extend(scan_text(path, root))

    if failures:
        print("\n".join(failures))
        return 1

    print("unicode guard passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
