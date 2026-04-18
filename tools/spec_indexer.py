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
    load_json_yaml,
    path_matches_any_glob,
    parse_moex_directory_listing,
    relative_posix_path,
    sha256_bytes,
    sha256_file,
    stable_id,
    utc_now_iso,
)


def build_row(
    root_spec: dict,
    canonical_url: str,
    upstream_modified: str,
    size: int,
    artifact_id: str,
    relative_path: str,
    status: str,
    relative_cache_path: str = "",
    sha256: str = "",
    required_artifact: dict | None = None,
    error: str = "",
    notes: str = "",
) -> dict:
    row = {
        "artifact_id": artifact_id,
        "root_artifact_id": root_spec["artifact_id"],
        "environment": root_spec["environment"],
        "canonical_url": canonical_url,
        "upstream_modified": upstream_modified,
        "retrieved_at": utc_now_iso(),
        "sha256": sha256,
        "size": size,
        "relative_path": relative_path,
        "relative_cache_path": relative_cache_path,
        "status": status,
    }
    if required_artifact:
        row["required_artifact"] = True
        row["required_expectation"] = required_artifact.get("expectation", "locked")
        if required_artifact.get("notes"):
            row["required_notes"] = required_artifact["notes"]
    if error:
        row["error"] = error
    if notes:
        row["notes"] = notes
    return row


def normalize_required_artifacts(root_spec: dict) -> dict[str, dict]:
    output: dict[str, dict] = {}
    for item in root_spec.get("required_artifacts", []):
        if isinstance(item, str):
            output[item] = {"relative_path": item, "expectation": "locked"}
        else:
            output[item["relative_path"]] = item
    return output


def should_consider(relative_path: str, include_globs: list[str], exclude_globs: list[str]) -> bool:
    if include_globs and not path_matches_any_glob(relative_path, include_globs):
        return False
    if exclude_globs and path_matches_any_glob(relative_path, exclude_globs):
        return False
    return True


def apply_large_file_policy(
    *,
    root_spec: dict,
    row_base: dict,
    relative_cache_path: str,
    required_artifact: dict | None,
    reason: str,
) -> dict:
    policy = root_spec.get("large_file_policy", "fetch")
    notes = f"policy_trigger={reason}; large_file_policy={policy}"
    if policy == "manifest_only":
        return build_row(
            root_spec=root_spec,
            canonical_url=row_base["canonical_url"],
            upstream_modified=row_base["upstream_modified"],
            size=row_base["size"],
            artifact_id=row_base["artifact_id"],
            relative_path=row_base["relative_path"],
            relative_cache_path=relative_cache_path,
            status="manifest_only",
            required_artifact=required_artifact,
            notes=notes,
        )
    if policy == "skip":
        return build_row(
            root_spec=root_spec,
            canonical_url=row_base["canonical_url"],
            upstream_modified=row_base["upstream_modified"],
            size=row_base["size"],
            artifact_id=row_base["artifact_id"],
            relative_path=row_base["relative_path"],
            status="skipped",
            required_artifact=required_artifact,
            notes=notes,
        )
    return {}


def lock_remote_root(root_spec: dict, workspace: Path) -> dict:
    source = root_spec["source"]
    cache_dir = workspace / Path(root_spec["cache_dir"])
    manifest_path = workspace / Path(root_spec["manifest"])
    ensure_dir(cache_dir)
    include_globs = list(root_spec.get("include_globs", []))
    exclude_globs = list(root_spec.get("exclude_globs", []))
    max_file_bytes = root_spec.get("max_file_bytes")
    max_total_bytes = root_spec.get("max_total_bytes")
    required_artifacts = normalize_required_artifacts(root_spec)
    seen_required: set[str] = set()
    fetched_total_bytes = 0
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
            required_artifact = required_artifacts.get(relative_path)
            if not should_consider(relative_path, include_globs, exclude_globs):
                if required_artifact:
                    rows.append(
                        build_row(
                            root_spec=root_spec,
                            canonical_url=entry["href"],
                            upstream_modified=entry["upstream_modified"],
                            size=entry["size"],
                            artifact_id=stable_id(root_spec["artifact_id"], relative_path),
                            relative_path=relative_path,
                            status="filtered_out",
                            required_artifact=required_artifact,
                            notes="excluded by include_globs/exclude_globs",
                        )
                    )
                    seen_required.add(relative_path)
                continue

            local_path = cache_dir / Path(relative_path)
            ensure_dir(local_path.parent)
            artifact_id = stable_id(root_spec["artifact_id"], relative_path)
            relative_cache_path = relative_posix_path(local_path.relative_to(workspace))
            row_base = {
                "canonical_url": entry["href"],
                "upstream_modified": entry["upstream_modified"],
                "size": entry["size"],
                "artifact_id": artifact_id,
                "relative_path": relative_path,
            }
            over_file_limit = max_file_bytes is not None and entry["size"] > int(max_file_bytes)
            over_total_limit = max_total_bytes is not None and fetched_total_bytes + entry["size"] > int(max_total_bytes)
            if over_file_limit:
                policy_row = apply_large_file_policy(
                    root_spec=root_spec,
                    row_base=row_base,
                    relative_cache_path=relative_cache_path,
                    required_artifact=required_artifact,
                    reason="max_file_bytes",
                )
                if policy_row:
                    rows.append(policy_row)
                    if required_artifact:
                        seen_required.add(relative_path)
                    continue
            if over_total_limit:
                policy_row = apply_large_file_policy(
                    root_spec=root_spec,
                    row_base=row_base,
                    relative_cache_path=relative_cache_path,
                    required_artifact=required_artifact,
                    reason="max_total_bytes",
                )
                if policy_row:
                    rows.append(policy_row)
                    if required_artifact:
                        seen_required.add(relative_path)
                    continue
            try:
                if local_path.exists() and local_path.stat().st_size == entry["size"]:
                    file_bytes = local_path.read_bytes()
                else:
                    file_bytes = fetch_bytes(entry["href"])
                    local_path.write_bytes(file_bytes)
                rows.append(
                    build_row(
                        root_spec=root_spec,
                        canonical_url=entry["href"],
                        upstream_modified=entry["upstream_modified"],
                        size=len(file_bytes),
                        artifact_id=artifact_id,
                        relative_path=relative_path,
                        relative_cache_path=relative_cache_path,
                        status="locked",
                        sha256=sha256_bytes(file_bytes),
                        required_artifact=required_artifact,
                    )
                )
                fetched_total_bytes += len(file_bytes)
                if required_artifact:
                    seen_required.add(relative_path)
            except (HTTPError, URLError) as error:
                rows.append(
                    build_row(
                        root_spec=root_spec,
                        canonical_url=entry["href"],
                        upstream_modified=entry["upstream_modified"],
                        size=entry["size"],
                        artifact_id=artifact_id,
                        relative_path=relative_path,
                        status="fetch_error",
                        required_artifact=required_artifact,
                        error=str(error),
                    )
                )
                if required_artifact:
                    seen_required.add(relative_path)

    missing_required = sorted(set(required_artifacts) - seen_required)
    for relative_path in missing_required:
        required_artifact = required_artifacts[relative_path]
        rows.append(
            build_row(
                root_spec=root_spec,
                canonical_url="",
                upstream_modified="",
                size=0,
                artifact_id=stable_id(root_spec["artifact_id"], relative_path),
                relative_path=relative_path,
                status="missing_required",
                required_artifact=required_artifact,
                notes="required artifact was not discovered in source listing",
            )
        )

    manifest = {
        "version": 1,
        "root": root_spec,
        "artifact_count": len(rows),
        "fetch_error_count": sum(1 for row in rows if row.get("status") == "fetch_error"),
        "manifest_only_count": sum(1 for row in rows if row.get("status") == "manifest_only"),
        "skipped_count": sum(1 for row in rows if row.get("status") == "skipped"),
        "fetched_total_bytes": fetched_total_bytes,
        "artifacts": sorted(rows, key=lambda item: item["artifact_id"]),
    }
    dump_json(manifest, manifest_path)
    return {"manifest": str(manifest_path.resolve()), "artifact_count": len(rows)}


def lock_local_root(root_spec: dict, workspace: Path, config_dir: Path) -> dict:
    source_path = Path(root_spec["source"])
    if not source_path.is_absolute():
        source_path = (config_dir / source_path).resolve()
    else:
        source_path = source_path.resolve()
    cache_dir = workspace / Path(root_spec["cache_dir"])
    manifest_path = workspace / Path(root_spec["manifest"])
    ensure_dir(cache_dir)
    include_globs = list(root_spec.get("include_globs", []))
    exclude_globs = list(root_spec.get("exclude_globs", []))
    max_file_bytes = root_spec.get("max_file_bytes")
    max_total_bytes = root_spec.get("max_total_bytes")
    required_artifacts = normalize_required_artifacts(root_spec)
    seen_required: set[str] = set()
    fetched_total_bytes = 0
    rows: list[dict] = []
    for file_path in iter_files(source_path):
        relative_path = relative_posix_path(file_path.relative_to(source_path))
        required_artifact = required_artifacts.get(relative_path)
        if not should_consider(relative_path, include_globs, exclude_globs):
            if required_artifact:
                rows.append(
                    build_row(
                        root_spec=root_spec,
                        canonical_url=file_url_from_path(file_path),
                        upstream_modified=str(int(file_path.stat().st_mtime)),
                        size=file_path.stat().st_size,
                        artifact_id=stable_id(root_spec["artifact_id"], relative_path),
                        relative_path=relative_path,
                        status="filtered_out",
                        required_artifact=required_artifact,
                        notes="excluded by include_globs/exclude_globs",
                    )
                )
                seen_required.add(relative_path)
            continue
        local_path = cache_dir / file_path.relative_to(source_path)
        artifact_id = stable_id(root_spec["artifact_id"], relative_path)
        relative_cache_path = relative_posix_path(local_path.relative_to(workspace))
        size = file_path.stat().st_size
        row_base = {
            "canonical_url": file_url_from_path(file_path),
            "upstream_modified": str(int(file_path.stat().st_mtime)),
            "size": size,
            "artifact_id": artifact_id,
            "relative_path": relative_path,
        }
        over_file_limit = max_file_bytes is not None and size > int(max_file_bytes)
        over_total_limit = max_total_bytes is not None and fetched_total_bytes + size > int(max_total_bytes)
        if over_file_limit:
            policy_row = apply_large_file_policy(
                root_spec=root_spec,
                row_base=row_base,
                relative_cache_path=relative_cache_path,
                required_artifact=required_artifact,
                reason="max_file_bytes",
            )
            if policy_row:
                rows.append(policy_row)
                if required_artifact:
                    seen_required.add(relative_path)
                continue
        if over_total_limit:
            policy_row = apply_large_file_policy(
                root_spec=root_spec,
                row_base=row_base,
                relative_cache_path=relative_cache_path,
                required_artifact=required_artifact,
                reason="max_total_bytes",
            )
            if policy_row:
                rows.append(policy_row)
                if required_artifact:
                    seen_required.add(relative_path)
                continue
        copy_file(file_path, local_path)
        rows.append(
            build_row(
                root_spec=root_spec,
                canonical_url=file_url_from_path(file_path),
                upstream_modified=str(int(file_path.stat().st_mtime)),
                size=size,
                artifact_id=artifact_id,
                relative_path=relative_path,
                relative_cache_path=relative_cache_path,
                status="locked",
                sha256=sha256_file(local_path),
                required_artifact=required_artifact,
            )
        )
        fetched_total_bytes += size
        if required_artifact:
            seen_required.add(relative_path)

    missing_required = sorted(set(required_artifacts) - seen_required)
    for relative_path in missing_required:
        required_artifact = required_artifacts[relative_path]
        rows.append(
            build_row(
                root_spec=root_spec,
                canonical_url="",
                upstream_modified="",
                size=0,
                artifact_id=stable_id(root_spec["artifact_id"], relative_path),
                relative_path=relative_path,
                status="missing_required",
                required_artifact=required_artifact,
                notes="required artifact was not found in local source tree",
            )
        )

    manifest = {
        "version": 1,
        "root": root_spec,
        "artifact_count": len(rows),
        "fetch_error_count": sum(1 for row in rows if row.get("status") == "fetch_error"),
        "manifest_only_count": sum(1 for row in rows if row.get("status") == "manifest_only"),
        "skipped_count": sum(1 for row in rows if row.get("status") == "skipped"),
        "fetched_total_bytes": fetched_total_bytes,
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
    config_dir = config_path.parent
    workspace = Path(args.workspace).resolve()
    config = load_json_yaml(config_path)
    results = []

    for root_spec in config["roots"]:
        if is_remote_source(root_spec["source"]):
            results.append(lock_remote_root(root_spec, workspace))
        else:
            results.append(lock_local_root(root_spec, workspace, config_dir))

    print(json.dumps({"locked_roots": results}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
