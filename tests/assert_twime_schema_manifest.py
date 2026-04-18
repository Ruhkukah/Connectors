#!/usr/bin/env python3
from __future__ import annotations

import json
import sys
from pathlib import Path


def sha256_file(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    project_root = Path(sys.argv[1]).resolve()
    manifest = json.loads((project_root / "protocols/twime_sbe/schema/schema.manifest.json").read_text(encoding="utf-8"))
    schema_path = project_root / "protocols/twime_sbe/schema/twime_spectra-7.7.xml"
    if sha256_file(schema_path) != manifest["sha256"]:
        raise SystemExit("schema sha256 does not match schema.manifest.json")
    if manifest["schema_id"] != 19781:
        raise SystemExit("unexpected schema_id in schema.manifest.json")
    if manifest["schema_version"] != 7:
        raise SystemExit("unexpected schema_version in schema.manifest.json")
    if manifest["byte_order"] != "littleEndian":
        raise SystemExit("unexpected byte_order in schema.manifest.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
