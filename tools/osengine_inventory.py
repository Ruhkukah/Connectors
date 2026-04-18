#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
from pathlib import Path

from moex_phase0_common import dump_json, ensure_dir, iter_files, stable_id, utc_now_iso


TYPE_RE = re.compile(r"\b(class|interface|enum|struct)\s+([A-Za-z_][A-Za-z0-9_]*)")
NAMESPACE_RE = re.compile(r"\bnamespace\s+([A-Za-z0-9_.]+)")
KEYWORD_RE = re.compile(r"\b(TWIME|FIX|FAST|Plaza|CGate|MassCancel|Replay|Recovery|RateGate|OrderBook|Trade)\b")


def analyze_root(root: Path) -> dict:
    file_entries = []
    type_entries = []
    keyword_hits: dict[str, int] = {}
    for file_path in iter_files(root):
        if file_path.suffix.lower() != ".cs":
            continue
        text = file_path.read_text(encoding="utf-8", errors="replace")
        namespace_match = NAMESPACE_RE.search(text)
        namespace_name = namespace_match.group(1) if namespace_match else ""
        file_entries.append(
            {
                "artifact_id": stable_id(root.name, file_path.relative_to(root).as_posix()),
                "path": str(file_path.resolve()),
                "relative_path": file_path.relative_to(root).as_posix(),
                "namespace": namespace_name,
            }
        )
        for kind, name in TYPE_RE.findall(text):
            type_entries.append(
                {
                    "type_id": stable_id(namespace_name, name),
                    "kind": kind,
                    "name": name,
                    "namespace": namespace_name,
                    "file": str(file_path.resolve()),
                }
            )
        for keyword in KEYWORD_RE.findall(text):
            keyword_hits[keyword] = keyword_hits.get(keyword, 0) + 1
    return {
        "root": str(root.resolve()),
        "files": file_entries,
        "types": type_entries,
        "keyword_hits": keyword_hits,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Inventory OsEngine connector directories without reusing implementation logic.")
    parser.add_argument("--root", action="append", required=True, help="Root directory to scan. May be supplied multiple times.")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    roots = [Path(item).resolve() for item in args.root]
    inventory = {
        "version": 1,
        "generated_at": utc_now_iso(),
        "roots": [analyze_root(root) for root in roots],
    }
    dump_json(inventory, Path(args.output).resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
