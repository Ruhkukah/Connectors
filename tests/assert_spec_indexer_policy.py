#!/usr/bin/env python3
from __future__ import annotations

import json
import sys
from pathlib import Path


def main() -> int:
    manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    rows = {row["relative_path"]: row for row in manifest["artifacts"]}

    assert "required.txt" in rows
    assert rows["required.txt"]["status"] == "locked"
    assert rows["required.txt"]["required_expectation"] == "locked"
    assert rows["required.txt"]["relative_cache_path"]
    assert "local_cache_path" not in rows["required.txt"]

    assert "large.bin" in rows
    assert rows["large.bin"]["status"] == "manifest_only"

    assert "later.txt" in rows
    assert rows["later.txt"]["status"] == "manifest_only"

    assert "excluded.txt" not in rows

    assert "missing.txt" in rows
    assert rows["missing.txt"]["status"] == "missing_required"
    assert rows["missing.txt"]["required_expectation"] == "needs_confirmation"

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
