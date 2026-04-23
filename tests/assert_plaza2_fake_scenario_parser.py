#!/usr/bin/env python3
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str((Path(sys.argv[1]).resolve() / "build" / "python-deps")))
sys.path.insert(0, str((Path(sys.argv[1]).resolve() / "tools")))

from plaza2_fake_scenario_common import (  # noqa: E402
    load_plaza2_fake_scenarios,
    load_plaza2_metadata_index,
    parse_plaza2_fake_scenario,
)


VALID_SCENARIO = """\
scenario_id: valid_scenario
description: Valid Phase 3D parser fixture

metadata:
  version: 1
  deterministic_seed: 0

streams:
  - name: FORTS_TRADE_REPL
    scope: private_core

events:
  - type: OPEN
  - type: TN_BEGIN
  - type: STREAM_DATA
    stream: FORTS_TRADE_REPL
    table: orders_log
    rows:
      - { replID: 1, private_amount: 2 }
  - type: TN_COMMIT

expected:
  invariants:
    - type: commit_count
      value: 1
"""

INVALID_EVENT_TYPE = VALID_SCENARIO.replace("STREAM_DATA", "MYSTERY_EVENT", 1)
INVALID_UNKNOWN_FIELD = VALID_SCENARIO.replace("private_amount", "qty", 1)
INVALID_MISSING_REPLID = VALID_SCENARIO.replace("{ replID: 1, private_amount: 2 }", "{ private_amount: 2 }", 1)
INVALID_STREAM_DATA_OUTSIDE_TX = VALID_SCENARIO.replace("  - type: TN_BEGIN\n", "", 1).replace(
    "  - type: TN_COMMIT\n", "", 1
)


def write_fixture(temp_dir: Path, name: str, content: str) -> Path:
    path = temp_dir / name
    stem = path.stem
    rendered = content.replace("scenario_id: valid_scenario", f"scenario_id: {stem}", 1)
    path.write_text(rendered, encoding="utf-8")
    return path


def expect_failure(path: Path, metadata_index: dict, snippet: str) -> None:
    try:
        parse_plaza2_fake_scenario(path, metadata_index)
    except ValueError as error:
        if snippet not in str(error):
            raise SystemExit(f"unexpected parser error for {path.name}: {error}")
        return
    raise SystemExit(f"expected parser failure for {path.name}")


def main() -> int:
    project_root = Path(sys.argv[1]).resolve()
    metadata_index = load_plaza2_metadata_index(
        project_root / "protocols" / "plaza2_cgate" / "generated" / "plaza2_generated_metadata.json"
    )
    scenarios = load_plaza2_fake_scenarios(project_root / "tests" / "plaza2_cgate" / "scenarios", metadata_index)
    if len(scenarios) < 4:
        raise SystemExit("tracked Phase 3D fake-engine scenario set is unexpectedly small")

    with tempfile.TemporaryDirectory(prefix="plaza2-fake-scenario-parser-") as temp_dir_name:
        temp_dir = Path(temp_dir_name)
        valid_path = write_fixture(temp_dir, "valid_scenario.yaml", VALID_SCENARIO)
        parsed = parse_plaza2_fake_scenario(valid_path, metadata_index)
        if len(parsed["events"]) != 4:
            raise SystemExit("valid parser fixture should contain four events")

        expect_failure(write_fixture(temp_dir, "invalid_event_type.yaml", INVALID_EVENT_TYPE), metadata_index, "unsupported event type")
        expect_failure(write_fixture(temp_dir, "invalid_unknown_field.yaml", INVALID_UNKNOWN_FIELD), metadata_index, "unknown field")
        expect_failure(write_fixture(temp_dir, "invalid_missing_replid.yaml", INVALID_MISSING_REPLID), metadata_index, "missing required field")
        expect_failure(
            write_fixture(temp_dir, "invalid_stream_data_outside_tx.yaml", INVALID_STREAM_DATA_OUTSIDE_TX),
            metadata_index,
            "outside TN_BEGIN/TN_COMMIT",
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
