#!/usr/bin/env python3
from __future__ import annotations

import argparse
from html import unescape
import json
from pathlib import Path
import re
import tempfile

from moex_phase0_common import dump_json, load_json_yaml
from plaza2_schema_common import normalize_type_token


ANCHOR_RE = re.compile(r'<a name="(?P<name>[^"]+)"></a>')
SUMMARY_TABLE_RE = re.compile(
    r'<table[^>]*summary="Fields of table (?P<table>[^"]+)"[^>]*>(?P<body>.*?)</table>',
    re.DOTALL,
)
ROW_RE = re.compile(r"<tr>(?P<body>.*?)</tr>", re.DOTALL)
CELL_RE = re.compile(r"<td[^>]*>(?P<body>.*?)</td>", re.DOTALL)
TAG_RE = re.compile(r"<[^>]+>")
WHITESPACE_RE = re.compile(r"\s+")


def clean_html(fragment: str) -> str:
    text = TAG_RE.sub(" ", fragment)
    return WHITESPACE_RE.sub(" ", unescape(text).replace("\xa0", " ")).strip()


def extract_table_rows(
    html_text: str,
    anchor_positions: dict[str, int],
    anchor_name: str,
    stream_anchor_name: str,
    table_name: str,
) -> list[dict[str, str]]:
    position = anchor_positions.get(anchor_name.lower())
    if position is None:
        anchor_suffix = f"_{table_name.lower()}"
        fallback_matches = [
            value for key, value in anchor_positions.items()
            if key.endswith(anchor_suffix)
        ]
        if len(fallback_matches) == 1:
            position = fallback_matches[0]
        else:
            stream_key = f"stream_{stream_anchor_name}".lower()
            stream_position = anchor_positions.get(stream_key)
            if stream_position is None:
                raise ValueError(f"field table anchor {anchor_name} not found in locked p2gate_en.html")
            stream_positions = sorted(
                value for key, value in anchor_positions.items()
                if key.startswith("stream_") and value > stream_position
            )
            stream_end = stream_positions[0] if stream_positions else len(html_text)
            window = html_text[stream_position:stream_end]
            summary_pattern = re.compile(
                rf'<table[^>]*summary="Fields of table {re.escape(table_name)}"[^>]*>(?P<body>.*?)</table>',
                re.DOTALL,
            )
            table_match = summary_pattern.search(window)
            if table_match is None:
                raise ValueError(f"field table anchor {anchor_name} not found in locked p2gate_en.html")
            rows: list[dict[str, str]] = []
            for row_match in ROW_RE.finditer(table_match.group("body")):
                cells = CELL_RE.findall(row_match.group("body"))
                if len(cells) != 3:
                    continue
                field_name = clean_html(cells[0])
                if not field_name:
                    continue
                rows.append(
                    {
                        "field_name": field_name,
                        "type_token": normalize_type_token(clean_html(cells[1])),
                        "description": clean_html(cells[2]),
                    }
                )
            if not rows:
                raise ValueError(f"no field rows extracted for anchor {anchor_name}")
            return rows

    window = html_text[position:]
    table_match = SUMMARY_TABLE_RE.search(window)
    if table_match is None:
        raise ValueError(f"field table body missing after anchor {anchor_name}")

    rows: list[dict[str, str]] = []
    for row_match in ROW_RE.finditer(table_match.group("body")):
        cells = CELL_RE.findall(row_match.group("body"))
        if len(cells) != 3:
            continue
        field_name = clean_html(cells[0])
        if not field_name:
            continue
        rows.append(
            {
                "field_name": field_name,
                "type_token": normalize_type_token(clean_html(cells[1])),
                "description": clean_html(cells[2]),
            }
        )
    if not rows:
        raise ValueError(f"no field rows extracted for anchor {anchor_name}")
    return rows


def write_if_different(destination: Path, text: str, check: bool) -> None:
    existing = destination.read_text(encoding="utf-8") if destination.exists() else None
    if existing == text:
        return
    if check:
        raise SystemExit(f"generated file is stale: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text, encoding="utf-8")


def build_reviewed_ini(
    streams: list[dict],
    tables: list[dict],
    manifest_row: dict,
    field_rows_by_table: dict[tuple[str, str], list[dict[str, str]]],
) -> str:
    lines = [
        "[meta]",
        "version = 1",
        "schema_name = plaza2_phase3b_reviewed",
        f"source_artifact_id = {manifest_row['artifact_id']}",
        f"source_relative_path = {manifest_row['relative_cache_path']}",
        f"source_sha256 = {manifest_row['sha256']}",
        "streams_inventory_path = matrix/protocol_inventory/plaza2_streams.yaml",
        "tables_inventory_path = matrix/protocol_inventory/plaza2_tables.yaml",
        "",
    ]

    for stream_order, stream in enumerate(streams, start=1):
        section_name = f"stream:{stream['stream_name']}"
        lines.extend(
            [
                f"[{section_name}]",
                f"order = {stream_order}",
                f"protocol_item_id = {stream['protocol_item_id']}",
                f"stream_name = {stream['stream_name']}",
                f"stream_anchor_name = {stream['stream_anchor_name']}",
                f"stream_type = {stream['stream_type']}",
                f"title = {stream['title']}",
                f"scope_bucket = {stream['scope_bucket']}",
                f"scheme_filename = {stream['scheme_filename']}",
                f"scheme_section = {stream['scheme_section']}",
                f"matching_partitioned = {'true' if stream['matching_partitioned'] else 'false'}",
                f"login_subtypes = {','.join(stream.get('login_subtypes', []))}",
                f"stream_variants = {','.join(stream.get('stream_variants', []))}",
                f"default_variant = {stream.get('default_variant', '')}",
                "",
            ]
        )

    for table_order, table in enumerate(tables, start=1):
        section_name = f"table:{table['stream_name']}.{table['table_name']}"
        lines.extend(
            [
                f"[{section_name}]",
                f"order = {table_order}",
                f"protocol_item_id = {table['protocol_item_id']}",
                f"stream_name = {table['stream_name']}",
                f"table_name = {table['table_name']}",
                f"title = {table['title']}",
                f"scope_bucket = {table['scope_bucket']}",
                "",
            ]
        )

    field_order = 1
    for table in tables:
        for row in field_rows_by_table[(table["stream_name"], table["table_name"])]:
            section_name = f"field:{table['stream_name']}.{table['table_name']}.{row['field_name']}"
            lines.extend(
                [
                    f"[{section_name}]",
                    f"order = {field_order}",
                    f"stream_name = {table['stream_name']}",
                    f"table_name = {table['table_name']}",
                    f"field_name = {row['field_name']}",
                    f"type_token = {row['type_token']}",
                    f"description = {row['description']}",
                    "",
                ]
            )
            field_order += 1

    return "\n".join(lines).rstrip() + "\n"


def build_manifest(
    manifest_row: dict,
    field_rows_by_table: dict[tuple[str, str], list[dict[str, str]]],
    output_relative_path: str,
    streams: list[dict],
    tables: list[dict],
) -> dict:
    field_count = sum(len(rows) for rows in field_rows_by_table.values())
    return {
        "version": 1,
        "schema_kind": "plaza2_reviewed_ini",
        "source_artifact_id": manifest_row["artifact_id"],
        "source_relative_path": manifest_row["relative_cache_path"],
        "source_sha256": manifest_row["sha256"],
        "source_upstream_modified": manifest_row["upstream_modified"],
        "streams_inventory_path": "matrix/protocol_inventory/plaza2_streams.yaml",
        "tables_inventory_path": "matrix/protocol_inventory/plaza2_tables.yaml",
        "output_relative_path": output_relative_path,
        "stream_count": len(streams),
        "table_count": len(tables),
        "field_count": field_count,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Materialize the reviewed Phase 3B PLAZA II scheme fixture from the locked docs.")
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--out-dir")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    project_root = Path(args.project_root).resolve()
    output_root = Path(args.out_dir).resolve() if args.out_dir else project_root

    docs_manifest_path = project_root / "spec-lock" / "prod" / "plaza2" / "docs" / "manifest.json"
    docs_manifest = load_json_yaml(docs_manifest_path)
    manifest_rows = {row["relative_path"]: row for row in docs_manifest["artifacts"]}
    html_manifest_row = manifest_rows.get("p2gate_en.html")
    if html_manifest_row is None:
        raise SystemExit("spec-lock/prod/plaza2/docs/manifest.json is missing p2gate_en.html")

    html_path = project_root / html_manifest_row["relative_cache_path"]
    html_text = html_path.read_text(encoding="utf-8", errors="replace")
    anchor_positions = {
        match.group("name").lower(): match.start()
        for match in ANCHOR_RE.finditer(html_text)
    }

    streams_inventory = load_json_yaml(project_root / "matrix" / "protocol_inventory" / "plaza2_streams.yaml")["items"]
    tables_inventory = load_json_yaml(project_root / "matrix" / "protocol_inventory" / "plaza2_tables.yaml")["items"]
    streams = [item for item in streams_inventory if item.get("kind") == "stream"]
    stream_anchor_by_name = {item["stream_name"]: item["stream_anchor_name"] for item in streams}

    field_rows_by_table: dict[tuple[str, str], list[dict[str, str]]] = {}
    for table in tables_inventory:
        stream_name = table["stream_name"]
        table_name = table["table_name"]
        stream_anchor_name = stream_anchor_by_name[stream_name]
        anchor_name = f"table_{stream_anchor_name}_{table_name}"
        rows = extract_table_rows(
            html_text,
            anchor_positions,
            anchor_name,
            stream_anchor_name,
            table_name,
        )
        field_rows_by_table[(stream_name, table_name)] = rows

    reviewed_ini_text = build_reviewed_ini(streams, tables_inventory, html_manifest_row, field_rows_by_table)
    reviewed_ini_path = output_root / "protocols" / "plaza2_cgate" / "schema" / "plaza2_forts_reviewed.ini"
    write_if_different(reviewed_ini_path, reviewed_ini_text, args.check)

    relative_output_path = reviewed_ini_path.relative_to(output_root).as_posix()
    manifest_payload = build_manifest(
        html_manifest_row,
        field_rows_by_table,
        relative_output_path,
        streams,
        tables_inventory,
    )

    manifest_path = output_root / "protocols" / "plaza2_cgate" / "schema" / "schema.manifest.json"
    with tempfile.TemporaryDirectory(prefix="plaza2-phase3b-schema-") as temp_dir:
        temp_manifest = Path(temp_dir) / "schema.manifest.json"
        dump_json(manifest_payload, temp_manifest)
        manifest_text = temp_manifest.read_text(encoding="utf-8")
    write_if_different(manifest_path, manifest_text, args.check)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
