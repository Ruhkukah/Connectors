#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path
import os


def main() -> int:
    project_root = Path(sys.argv[1]).resolve()
    env = dict(os.environ)
    env["PYTHONPATH"] = str(project_root / "build" / "python-deps")
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
