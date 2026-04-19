#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess


SOURCE_SUFFIXES = {".cpp", ".hpp", ".py"}
HANDWRITTEN_CPP_SUFFIXES = {".cpp", ".hpp"}


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


def iter_source_files(paths: list[Path]) -> list[Path]:
    discovered: list[Path] = []
    for path in paths:
        if path.is_file():
            if path.suffix in SOURCE_SUFFIXES:
                discovered.append(path)
            continue
        for candidate in sorted(path.rglob("*")):
            if candidate.is_file() and candidate.suffix in SOURCE_SUFFIXES:
                discovered.append(candidate)
    return discovered


def main() -> int:
    parser = argparse.ArgumentParser(description="Check TWIME source/style invariants without rewriting files.")
    parser.add_argument("--require-clang-format", action="store_true")
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args()

    failures: list[str] = []
    source_files = iter_source_files([Path(item).resolve() for item in args.paths])
    for path in source_files:
        raw = path.read_bytes()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as error:
            failures.append(f"{path}: utf-8 decode error: {error}")
            continue
        lines = text.splitlines()
        nonempty = [line for line in lines if line.strip()]
        if len(nonempty) <= 2 and any(len(line) > 200 for line in nonempty):
            failures.append(f"{path}: appears minified")
        if any("\t" in line for line in lines):
            failures.append(f"{path}: contains tab indentation")

        if b"\r" in raw.replace(b"\r\n", b""):
            failures.append(f"{path}: contains CR-only line endings")

        if path.suffix in HANDWRITTEN_CPP_SUFFIXES and "generated" not in path.parts:
            blank_lines = len(lines) - len(nonempty)
            if blank_lines > max(20, len(nonempty) // 2):
                failures.append(f"{path}: excessive blank-line ratio")
            consecutive_blank = 0
            for line in lines:
                if line.strip():
                    consecutive_blank = 0
                    continue
                consecutive_blank += 1
                if consecutive_blank >= 3:
                    failures.append(f"{path}: contains 3 or more consecutive blank lines")
                    break

        max_length = 320 if "generated" in path.parts else 160
        for index, line in enumerate(lines, start=1):
            if len(line) > max_length:
                failures.append(f"{path}:{index}: line exceeds {max_length} characters")
                break

    clang_format = find_clang_format()
    if args.require_clang_format and clang_format is None:
        failures.append("clang-format is required but not installed")
    if clang_format is not None:
        cpp_files = [
            str(path)
            for path in source_files
            if path.suffix in {".cpp", ".hpp"} and "generated" not in path.parts
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
