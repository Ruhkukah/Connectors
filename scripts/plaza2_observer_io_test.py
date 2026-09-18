#!/usr/bin/env python3
"""50k-row actual journal, abrupt process death and streaming reader fault checks."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("observer_read", ROOT / "tools/plaza2_observer_read.py")
READER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(READER)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="moex-observer-io-") as root:
        subprocess.run([binary, str(Path(root) / "bounds.jsonl"), "bounds"], check=True, timeout=5)
        clean = Path(root) / "clean.jsonl"
        result = subprocess.run([binary, str(clean), "clean"], check=True, capture_output=True,
                                text=True, timeout=60)
        metrics = json.loads(result.stdout)
        assert metrics["row_syncs"] == 0 and metrics["commit_syncs"] == 1
        assert metrics["sync_calls"] == 4  # OPEN, COMMIT, ONLINE, clean shutdown; directory sync separate
        assert metrics["peak_buffer_bytes"] <= 65536
        assert metrics["write_calls"] < 5000, metrics  # not one syscall per row
        assert metrics["bytes"] == clean.stat().st_size
        summary = READER.inspect(clean)
        assert summary["committed_rows"] == 50000 and summary["committed_transactions"] == 1
        assert summary["incomplete_rows"] == 0 and summary["clean_shutdown"]
        seen = 0
        with clean.open(encoding="utf-8") as source:
            for line in source:
                event = json.loads(line)
                if event["kind"] != "row":
                    continue
                assert event["fields"]["sess_id"]["value"] == seen
                assert bytes.fromhex(event["raw_payload_hex"])[0] == seen & 255
                assert event["fields"]["begin"]["p2time_components"] == {
                    "year": 2026, "month": 9, "day": 18, "hour": 19, "minute": 5,
                    "second": 42, "millisecond": 123}
                seen += 1
        assert seen == 50000
        crash = Path(root) / "crash.jsonl"
        result = subprocess.run([binary, str(crash), "crash"], capture_output=True, text=True, timeout=60)
        assert result.returncode == 23
        assert json.loads(result.stdout)["row_syncs"] == 0
        incomplete = READER.inspect(crash)
        assert incomplete["committed_rows"] == 0
        assert incomplete["incomplete_transactions"] == 1
        assert 0 < incomplete["incomplete_rows"] <= 50000
        assert not incomplete["clean_shutdown"]
        # Corrupt commit hash, missing begin and a torn final marker never prove a commit.
        fault = Path(root) / "fault.jsonl"
        begin = dict(seq=1, kind="transaction_begin", generation=1, stream="S", transaction_id=1)
        row = dict(seq=2, kind="row", generation=1, stream="S", transaction_id=1)
        commit = dict(seq=3, kind="transaction_commit", generation=1, stream="S", transaction_id=1,
                      transaction_committed=True, recorded_row_count=1, source_row_count=1,
                      recorded_row_fnv1a64="0")
        fault.write_text("".join(json.dumps(x) + "\n" for x in (begin, row, commit)), encoding="utf-8")
        try:
            READER.inspect(fault)
            raise AssertionError("corrupt hash accepted")
        except ValueError as error:
            assert "hash mismatch" in str(error)
        fault.write_text(json.dumps(begin) + "\n" + json.dumps(row) + "\n" + '{"seq":3', encoding="utf-8")
        torn = READER.inspect(fault)
        assert torn["torn_tail"] and torn["committed_rows"] == 0 and torn["incomplete_rows"] == 1
        fault.write_text(json.dumps(dict(row, seq=1)) + "\n", encoding="utf-8")
        try:
            READER.inspect(fault)
            raise AssertionError("row without begin accepted")
        except ValueError as error:
            assert "matching transaction begin" in str(error)
        print(json.dumps(dict(stress=metrics, committed=summary, crash=incomplete), sort_keys=True))


if __name__ == "__main__":
    main()
