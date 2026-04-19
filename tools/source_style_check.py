#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess

from style_common import (
    MAX_LINE_LENGTH,
    SOURCE_STYLE_DEFAULT_EXCLUDES,
    classify_kind,
    excessive_blank_line_issue,
    has_cr_only,
    has_final_newline,
    is_generated_file,
    is_generated_path,
    is_minified_like,
    matches_any,
    repo_relative,
    tracked_files,
)


HANDWRITTEN_CPP_SUFFIXES = {".cpp", ".hpp", ".h"}


def find_clang_format() -> str | None:
    clang_format = shutil.which("clang-format")
    if clang_format is not None:
        return clang_format

    xcrun = shutil.which("xcrun")
    if xcrun is None:
        return None

    result = subprocess.run(
        [xcrun, "--find", "clang-format"],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return None
    candidate = result.stdout.strip()
    return candidate or None


def discover_files(paths: list[str], root: Path) -> list[Path]:
    discovered: list[Path] = []
    seen: set[Path] = set()
    for candidate in tracked_files(root, paths):
        resolved = candidate.resolve()
        relative = repo_relative(resolved, root)
        if matches_any(relative, SOURCE_STYLE_DEFAULT_EXCLUDES):
            continue
        kind = classify_kind(resolved)
        if kind is None and not is_generated_path(resolved):
            continue
        if resolved not in seen:
            discovered.append(resolved)
            seen.add(resolved)
    return discovered


def check_text_file(path: Path, root: Path) -> list[str]:
    raw = path.read_bytes()
    relative = repo_relative(path, root)

    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        return [f"{relative}: utf-8 decode error: {error}"]

    generated = is_generated_file(path, text)
    failures: list[str] = []

    if is_generated_path(path) and not generated:
        failures.append(f"{relative}: generated-path file is missing a generated marker")

    if not has_final_newline(raw):
        failures.append(f"{relative}: missing final newline")

    if has_cr_only(raw):
        failures.append(f"{relative}: contains CR-only line endings")

    if not generated and is_minified_like(path, text):
        failures.append(f"{relative}: appears minified")

    blank_line_issue = excessive_blank_line_issue(path, text)
    if blank_line_issue is not None:
        failures.append(f"{relative}: {blank_line_issue}")

    kind = classify_kind(path) or "text"
    max_length = 320 if generated else MAX_LINE_LENGTH[kind]
    for index, line in enumerate(text.splitlines(), start=1):
        if len(line) > max_length:
            failures.append(f"{relative}:{index}: line exceeds {max_length} characters")
            break

    return failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check repo source/config/docs style invariants without rewriting files."
    )
    parser.add_argument("--require-clang-format", action="store_true")
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args()

    root = Path.cwd().resolve()
    files = discover_files(args.paths, root)
    failures: list[str] = []

    for path in files:
        failures.extend(check_text_file(path, root))

    clang_format = find_clang_format()
    if args.require_clang_format and clang_format is None:
        failures.append("clang-format is required but not installed")

    if clang_format is not None:
        cpp_files = [
            str(path)
            for path in files
            if path.suffix in HANDWRITTEN_CPP_SUFFIXES and not is_generated_path(path)
        ]
        if cpp_files:
            result = subprocess.run(
                [clang_format, "--dry-run", "--Werror", *cpp_files],
                capture_output=True,
                text=True,
                check=False,
            )
            if result.returncode != 0:
                stderr = result.stderr.strip() or result.stdout.strip() or "clang-format reported differences"
                failures.append(stderr)

    if failures:
        for failure in failures:
            print(failure)
        return 1

    print("source style check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
