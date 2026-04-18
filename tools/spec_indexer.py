#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
from urllib.error import HTTPError, URLError

from moex_phase0_common import (
    PROJECT_ROOT,
    copy_file,
    dump_json,
    ensure_dir,
    fetch_bytes,
    fetch_text,
    file_url_from_path,
    is_remote_source,
    iter_files,
    parse_moex_directory_listing,
    relative_posix_path,
    sha256_bytes,
    sha256_file,
    stable_id,
    utc_now_iso,
)


def build_row(root_spec: dict, canonical_url: str, upstream_modified: str, size: int, local_path: Path, artifact_id: str, relative_path: str) -> dict:
    return {
        "artifact_id": artifact_id,
        "root_artifact_id": root_spec["artifact_id"],
        "environment": root_spec["environment"],
        "canonical_url": canonical_url,
        "upstream_modified": upstream_modified,
        "retrieved_at": utc_now_iso(),
        "sha256": sha256_file(local_path),
        "size": size,
        "relative_path": relative_path,
        "local_cache_path": str(local_path.resolve()),
    }


def lock_remote_root(root_spec: dict, workspace: Path) -> dict:
    source = root_spec["source"]
    cache_dir = workspace / Path(root_spec["cache_dir"])
    manifest_path = workspace / Path(root_spec["manifest"])
    ensure_dir(cache_dir)
    queue: list[tuple[str, str]] = [(source, "")]
    rows: list[dict] = []

    while queue:
        current_url, relative_dir = queue.pop(0)
        html = fetch_text(current_url)
        for entry in parse_moex_directory_listing(current_url, html):
            if entry["is_dir"]:
                queue.append((entry["href"], f"{relative_dir}{entry['name']}/"))
                continue

            relative_path = f"{relative_dir}{entry['name']}"
            local_path = cache_dir / Path(relative_path)
            ensure_dir(local_path.parent)
            artifact_id = stable_id(root_spec["artifact_id"], relative_path)
            try:
                if local_path.exists() and local_path.stat().st_size == entry["size"]:
                    file_bytes = local_path.read_bytes()
                else:
                    file_bytes = fetch_bytes(entry["href"])
                    local_path.write_bytes(file_bytes)
                rows.append(
                    {
                        "artifact_id": artifact_id,
                        "root_artifact_id": root_spec["artifact_id"],
                        "environment": root_spec["environment"],
                        "canonical_url": entry["href"],
                        "upstream_modified": entry["upstream_modified"],
                        "retrieved_at": utc_now_iso(),
                        "sha256": sha256_bytes(file_bytes),
                        "size": len(file_bytes),
                        "relative_path": relative_path,
                        "local_cache_path": str(local_path.resolve()),
                        "status": "locked",
                    }
                )
            except (HTTPError, URLError) as error:
                rows.append(
                    {
                        "artifact_id": artifact_id,
                        "root_artifact_id": root_spec["artifact_id"],
                        "environment": root_spec["environment"],
                        "canonical_url": entry["href"],
                        "upstream_modified": entry["upstream_modified"],
                        "retrieved_at": utc_now_iso(),
                        "sha256": "",
                        "size": entry["size"],
                        "relative_path": relative_path,
                        "local_cache_path": "",
                        "status": "fetch_error",
                        "error": str(error),
                    }
                )

    manifest = {
        "version": 1,
        "root": root_spec,
        "artifact_count": len(rows),
        "fetch_error_count": sum(1 for row in rows if row.get("status") != "locked"),
        "artifacts": sorted(rows, key=lambda item: item["artifact_id"]),
    }
    dump_json(manifest, manifest_path)
    return {"manifest": str(manifest_path.resolve()), "artifact_count": len(rows)}


def lock_local_root(root_spec: dict, workspace: Path) -> dict:
    source_path = Path(root_spec["source"]).resolve()
    cache_dir = workspace / Path(root_spec["cache_dir"])
    manifest_path = workspace / Path(root_spec["manifest"])
    ensure_dir(cache_dir)
    rows: list[dict] = []
    for file_path in iter_files(source_path):
        relative_path = relative_posix_path(file_path.relative_to(source_path))
        local_path = cache_dir / file_path.relative_to(source_path)
        copy_file(file_path, local_path)
        artifact_id = stable_id(root_spec["artifact_id"], relative_path)
        rows.append(
            build_row(
                root_spec=root_spec,
                canonical_url=file_url_from_path(file_path),
                upstream_modified=str(int(file_path.stat().st_mtime)),
                size=file_path.stat().st_size,
                local_path=local_path,
                artifact_id=artifact_id,
                relative_path=relative_path,
            )
        )

    manifest = {
        "version": 1,
        "root": root_spec,
        "artifact_count": len(rows),
        "artifacts": sorted(rows, key=lambda item: item["artifact_id"]),
    }
    dump_json(manifest, manifest_path)
    return {"manifest": str(manifest_path.resolve()), "artifact_count": len(rows)}


def main() -> int:
    parser = argparse.ArgumentParser(description="Lock remote or local artifact roots into hashed manifests.")
    parser.add_argument("--config", required=True, help="Path to JSON/YAML config describing roots to lock.")
    parser.add_argument("--workspace", default=str(PROJECT_ROOT), help="Workspace root where cache and manifests are written.")
    args = parser.parse_args()

    config_path = Path(args.config).resolve()
    workspace = Path(args.workspace).resolve()
    config = __import__("json").loads(config_path.read_text(encoding="utf-8"))
    results = []

    for root_spec in config["roots"]:
        if is_remote_source(root_spec["source"]):
            results.append(lock_remote_root(root_spec, workspace))
        else:
            results.append(lock_local_root(root_spec, workspace))

    print(json.dumps({"locked_roots": results}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
