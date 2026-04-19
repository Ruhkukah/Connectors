#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from style_common import (
    MAX_LINE_LENGTH,
    REPO_STYLE_DEFAULT_EXCLUDES,
    classify_kind,
    has_cr_only,
    has_final_newline,
    is_binary_extension,
    is_generated_file,
    is_generated_path,
    is_minified_like,
    matches_any,
    repo_relative,
    tracked_files,
)
from unicode_guard import scan_text


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Scan tracked repo files for minified formatting, newline, and Unicode issues."
    )
    parser.add_argument(
        "--paths",
        nargs="*",
        help="Optional tracked paths to scan instead of the whole repository.",
    )
    parser.add_argument(
        "--include-excluded",
        action="store_true",
        help="Include files that are excluded by the default repo-style policy.",
    )
    return parser.parse_args()


def discover_files(root: Path, path_specs: list[str] | None) -> list[Path]:
    if not path_specs:
        return [path for path in tracked_files(root) if path.is_file()]

    discovered: list[Path] = []
    seen: set[Path] = set()

    for raw in path_specs:
        candidate = Path(raw)
        if candidate.exists():
            resolved = candidate.resolve()
            if resolved.is_file() and resolved not in seen:
                discovered.append(resolved)
                seen.add(resolved)
                continue
            if resolved.is_dir():
                for nested in sorted(resolved.rglob("*")):
                    if nested.is_file():
                        nested = nested.resolve()
                        if nested not in seen:
                            discovered.append(nested)
                            seen.add(nested)
                continue

    for path in tracked_files(root, path_specs):
        resolved = path.resolve()
        if resolved.is_file() and resolved not in seen:
            discovered.append(resolved)
            seen.add(resolved)

    return discovered


def main() -> int:
    args = parse_args()
    root = Path.cwd().resolve()
    failures: list[str] = []

    for path in discover_files(root, args.paths):
        if not path.is_file():
            continue

        relative = repo_relative(path, root)
        if not args.include_excluded and matches_any(relative, REPO_STYLE_DEFAULT_EXCLUDES):
            continue
        if is_binary_extension(path):
            continue

        raw = path.read_bytes()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as error:
            failures.append(f"{relative}: utf-8 decode error: {error}")
            continue

        failures.extend(scan_text(path, root))

        if not has_final_newline(raw):
            failures.append(f"{relative}: missing final newline")

        if has_cr_only(raw):
            failures.append(f"{relative}: contains CR-only line endings")

        generated = is_generated_file(path, text)
        if is_generated_path(path) and not generated:
            failures.append(f"{relative}: generated-path file is missing a generated marker")

        if not generated and is_minified_like(path, text):
            failures.append(f"{relative}: appears minified")

        kind = classify_kind(path)
        if kind is None:
            continue

        max_length = 320 if generated else MAX_LINE_LENGTH[kind]
        for index, line in enumerate(text.splitlines(), start=1):
            if len(line) > max_length:
                failures.append(f"{relative}:{index}: line exceeds {max_length} characters")
                break

    if failures:
        for failure in failures:
            print(failure)
        return 1

    print("repo style check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
