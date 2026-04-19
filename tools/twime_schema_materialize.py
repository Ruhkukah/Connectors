#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from moex_phase0_common import copy_file, dump_json, ensure_dir, load_json_yaml, sha256_file


def _find_required_row(manifest: dict, relative_path: str) -> dict:
    for row in manifest["artifacts"]:
        if row["relative_path"] == relative_path:
            return row
    raise ValueError(f"Required TWIME artifact missing from manifest: {relative_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Materialize pinned TWIME schema artifacts from local spec-lock cache.")
    parser.add_argument("--manifest", default="spec-lock/prod/twime/manifest.json")
    parser.add_argument("--output-dir", default="protocols/twime_sbe/schema")
    args = parser.parse_args()

    manifest_path = Path(args.manifest).resolve()
    project_root = manifest_path.parents[3]
    manifest = load_json_yaml(manifest_path)
    output_dir = ensure_dir((project_root / args.output_dir).resolve())

    schema_row = _find_required_row(manifest, "twime_spectra-7.7.xml")
    source_schema = project_root / schema_row["relative_cache_path"]
    if not source_schema.exists():
        raise FileNotFoundError(
            f"Pinned TWIME schema cache is missing: {source_schema}. "
            f"Run spec_indexer first to materialize the schema into spec-lock."
        )
    if sha256_file(source_schema) != schema_row["sha256"]:
        raise ValueError(f"SHA-256 mismatch for {source_schema}")

    destination_schema = output_dir / "twime_spectra-7.7.xml"
    copy_file(source_schema, destination_schema)

    xsl_row = None
    for row in manifest["artifacts"]:
        if row["relative_path"] == "sbe_schema.xsl":
            xsl_row = row
            break
    if xsl_row:
        source_xsl = project_root / xsl_row["relative_cache_path"]
        if source_xsl.exists() and sha256_file(source_xsl) == xsl_row["sha256"]:
            copy_file(source_xsl, output_dir / "sbe_schema.xsl")

    schema_manifest = {
        "source_artifact_id": schema_row["artifact_id"],
        "canonical_url": schema_row["canonical_url"],
        "sha256": schema_row["sha256"],
        "size": schema_row["size"],
        "retrieved_at": schema_row["retrieved_at"],
        "active_schema": True,
        "schema_id": 19781,
        "schema_version": 7,
        "byte_order": "littleEndian",
        "package": "moex_spectra_twime",
    }
    dump_json(schema_manifest, output_dir / "schema.manifest.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
