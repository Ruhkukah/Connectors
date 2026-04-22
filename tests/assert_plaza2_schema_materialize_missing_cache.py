#!/usr/bin/env python3
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def copy_file(project_root: Path, temp_root: Path, relative_path: str) -> None:
    source = project_root / relative_path
    destination = temp_root / relative_path
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def main() -> int:
    project_root = Path(sys.argv[1]).resolve()
    materializer = Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="plaza2-schema-check-no-cache-") as temp_dir_name:
        temp_root = Path(temp_dir_name)
        copy_file(project_root, temp_root, "spec-lock/prod/plaza2/docs/manifest.json")
        copy_file(project_root, temp_root, "protocols/plaza2_cgate/schema/plaza2_forts_reviewed.ini")
        copy_file(project_root, temp_root, "protocols/plaza2_cgate/schema/schema.manifest.json")

        result = subprocess.run(
            [
                str(materializer),
                "--project-root",
                str(temp_root),
                "--check",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise SystemExit(
                "plaza2_schema_materialize --check should succeed without docs cache\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
