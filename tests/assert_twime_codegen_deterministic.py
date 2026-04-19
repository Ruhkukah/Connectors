#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path
import os


def compose_pythonpath(project_root: Path) -> str:
    parts: list[str] = []
    existing = os.environ.get("PYTHONPATH", "")
    if existing:
        parts.extend([item for item in existing.split(os.pathsep) if item])

    fallback = project_root / "build" / "python-deps"
    if fallback.exists():
        fallback_text = str(fallback)
        if fallback_text not in parts:
            parts.append(fallback_text)

    return os.pathsep.join(parts)


def main() -> int:
    project_root = Path(sys.argv[1]).resolve()
    env = dict(os.environ)
    pythonpath = compose_pythonpath(project_root)
    if pythonpath:
        env["PYTHONPATH"] = pythonpath
    subprocess.run(
        [
            sys.executable,
            str(project_root / "tools" / "twime_codegen.py"),
            "--schema",
            str(project_root / "protocols" / "twime_sbe" / "schema" / "twime_spectra-7.7.xml"),
            "--out",
            str(project_root / "protocols" / "twime_sbe" / "generated"),
            "--check",
        ],
        check=True,
        cwd=project_root,
        env=env,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
