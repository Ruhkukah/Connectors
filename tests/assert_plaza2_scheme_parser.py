#!/usr/bin/env python3
from __future__ import annotations

import configparser
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str((Path(sys.argv[1]).resolve() / "build" / "python-deps")))
sys.path.insert(0, str((Path(sys.argv[1]).resolve() / "tools")))

from plaza2_schema_common import parse_reviewed_scheme  # noqa: E402


VALID_MINI_FIXTURE = """\
[meta]
version = 1
schema_name = parser_fixture
source_artifact_id = fixture
source_relative_path = tests/fixtures/plaza2/scheme/mini.ini
source_sha256 = 00
streams_inventory_path = matrix/protocol_inventory/plaza2_streams.yaml
tables_inventory_path = matrix/protocol_inventory/plaza2_tables.yaml

[stream:FORTS_TRADE_REPL]
order = 1
protocol_item_id = plaza2_stream_forts_trade_repl
stream_name = FORTS_TRADE_REPL
stream_anchor_name = FORTS_TRADE_REPL
stream_type = R
title = User's orders and trades
scope_bucket = private_core
scheme_filename = forts_scheme.ini
scheme_section = FORTS_TRADE_REPL
matching_partitioned = true
login_subtypes = main,viewing,transactional
stream_variants =
default_variant =

[table:FORTS_TRADE_REPL.orders_log]
order = 1
protocol_item_id = plaza2_table_forts_trade_repl_orders_log
stream_name = FORTS_TRADE_REPL
table_name = orders_log
title = Log of operations with orders
scope_bucket = private_core

[field:FORTS_TRADE_REPL.orders_log.replID]
order = 1
stream_name = FORTS_TRADE_REPL
table_name = orders_log
field_name = replID
type_token = i8
description = Service field of the replication subsystem
"""


INVALID_UNKNOWN_TYPE = VALID_MINI_FIXTURE.replace("type_token = i8", "type_token = q9")
INVALID_UNKNOWN_TABLE = VALID_MINI_FIXTURE.replace(
    "table_name = orders_log\nfield_name = replID",
    "table_name = missing_table\nfield_name = replID",
)
INVALID_DUPLICATE_SECTION = VALID_MINI_FIXTURE + "\n[table:FORTS_TRADE_REPL.orders_log]\norder = 2\n"


def write_fixture(temp_dir: Path, name: str, content: str) -> Path:
    path = temp_dir / name
    path.write_text(content, encoding="utf-8")
    return path


def expect_failure(path: Path, snippet: str) -> None:
    try:
        parse_reviewed_scheme(path)
    except (ValueError, configparser.Error) as error:
        if snippet not in str(error):
            raise SystemExit(f"unexpected parser error for {path.name}: {error}")
        return
    raise SystemExit(f"expected parser failure for {path.name}")


def main() -> int:
    project_root = Path(sys.argv[1]).resolve()
    parsed = parse_reviewed_scheme(project_root / "protocols" / "plaza2_cgate" / "schema" / "plaza2_forts_reviewed.ini")
    if parsed["schema"]["stream_count"] < 10:
        raise SystemExit("reviewed PLAZA II fixture parsed too few streams")
    if parsed["schema"]["field_count"] < 100:
        raise SystemExit("reviewed PLAZA II fixture parsed too few fields")

    with tempfile.TemporaryDirectory(prefix="plaza2-parser-") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        valid_path = write_fixture(temp_dir, "valid.ini", VALID_MINI_FIXTURE)
        mini = parse_reviewed_scheme(valid_path)
        if mini["schema"]["field_count"] != 1:
            raise SystemExit("mini parser fixture should contain exactly one field")

        expect_failure(write_fixture(temp_dir, "unknown_type.ini", INVALID_UNKNOWN_TYPE), "unsupported PLAZA II type token")
        expect_failure(write_fixture(temp_dir, "unknown_table.ini", INVALID_UNKNOWN_TABLE), "field references unknown table")
        expect_failure(write_fixture(temp_dir, "duplicate_section.ini", INVALID_DUPLICATE_SECTION), "section")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
