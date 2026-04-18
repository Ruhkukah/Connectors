#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from moex_phase0_common import dump_json, ensure_dir, iter_files, sha256_file, stable_id, utc_now_iso


def main() -> int:
    parser = argparse.ArgumentParser(description="Fingerprint a local PLAZA II/CGate scheme directory.")
    parser.add_argument("--scheme-dir", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--generated-dir")
    parser.add_argument("--cgate-version", default="unknown")
    parser.add_argument("--p2mqrouter-version", default="unknown")
    args = parser.parse_args()

    scheme_dir = Path(args.scheme_dir).resolve()
    output_path = Path(args.output).resolve()
    generated_dir = Path(args.generated_dir).resolve() if args.generated_dir else None

    files = []
    for root_name, root_dir in [("scheme", scheme_dir), ("generated", generated_dir)]:
        if root_dir is None or not root_dir.exists():
            continue
        for file_path in iter_files(root_dir):
            files.append(
                {
                    "artifact_id": stable_id(root_name, file_path.relative_to(root_dir).as_posix()),
                    "root_kind": root_name,
                    "relative_path": file_path.relative_to(root_dir).as_posix(),
                    "sha256": sha256_file(file_path),
                    "size": file_path.stat().st_size,
                }
            )

    manifest = {
        "version": 1,
        "retrieved_at": utc_now_iso(),
        "cgate_version": args.cgate_version,
        "p2mqrouter_version": args.p2mqrouter_version,
        "scheme_dir_name": scheme_dir.name,
        "generated_dir_name": generated_dir.name if generated_dir else "",
        "artifacts": files,
    }
    dump_json(manifest, output_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
