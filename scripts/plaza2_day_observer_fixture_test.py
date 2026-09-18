#!/usr/bin/env python3
"""Offline callback-journal contract tests; no CGate runtime or sockets."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="moex-day-observer-") as root:
        output = Path(root) / "events.jsonl"
        args = [binary, "--offline-fixture", "--output", str(output)]
        subprocess.run(args, check=True)
        raw = output.read_bytes()
        events = [json.loads(line) for line in raw.decode("utf-8").splitlines()]
        assert json.loads(json.dumps(events, ensure_ascii=False)) == events
        assert [e["seq"] for e in events] == list(range(1, len(events) + 1))
        assert all(e["receive_unix_ns"] > 0 and e["receive_steady_ns"] > 0 for e in events)
        assert output.stat().st_mode & 0o777 == 0o600
        aggr = [e for e in events if e.get("stream") == "FORTS_AGGR##_REPL"]
        # Metadata uses a depth placeholder for the FORTS_AGGR20_REPL wire service.
        assert aggr, {e.get("stream") for e in events}
        rows = [e for e in aggr if e["kind"] == "row"]
        commits = {e["transaction_id"]: e["transaction_committed"]
                   for e in aggr if e["kind"] == "transaction_commit"}
        assert [commits.get(e["transaction_id"], False) for e in rows] == [True, True, False, False, True]
        assert [e["online_before"] for e in rows] == [False, True, True, True, True]
        assert rows[0]["transaction_id"] == 10001 and rows[0]["transaction_row_index"] == 2
        assert len(events) < 60 and len(raw) < 30000  # 10,000 book-only transactions suppressed
        begins = [e for e in aggr if e["kind"] == "transaction_begin"]
        assert len(begins) == 5 and all(e["deferred_begin"] for e in begins)
        assert next(e for e in aggr if e["kind"] == "transaction_commit")["source_row_count"] == 2
        diagnostics = [e for e in aggr if e["kind"] == "callback_error"]
        assert diagnostics[0]["message"] == "bad fixed string"
        assert "secret" not in diagnostics[1]["message"] and "redacted" in diagnostics[1]["message"]
        for row in rows:
            fields = row["fields"]
            assert fields["event_id"]["value"] == 678984
            assert fields["sess_id"]["value"] == 11709
            assert fields["event_type"]["value"] == 1
            assert fields["server_time"]["value"] == 123456
            assert fields["message"]["value"] == "session_data_ready"
        assert {"online", "lifenum", "clear_deleted", "close"} <= {e["kind"] for e in aggr}
        for stream in ("FORTS_REFDATA_REPL", "FORTS_SESSIONSTATE_REPL", "FORTS_INSTRUMENTSTATE_REPL"):
            subset = [e for e in events if e.get("stream") == stream]
            assert [e["kind"] for e in subset] == [
                "open", "transaction_begin", "row", "transaction_commit", "online", "close"]
            assert subset[3]["transaction_committed"]
        names = [e["fields"]["name"] for e in events if e["kind"] == "row" and "name" in e["fields"]]
        assert len(names) == 1
        expected = "Фьючерсный контракт ALRS-12.26"
        assert names[0]["value"] == expected
        assert bytes.fromhex(names[0]["raw_hex"]) == expected.encode("cp1251")
        text = next(e for e in events if e["kind"] == "fixture_text")
        assert text["undefined"] == "\ufffd" and text["escaped"] == '"\\\n\r\t'
        # An existing evidence file must never be overwritten.
        assert subprocess.run(args, capture_output=True).returncode == 1
        assert output.read_bytes() == raw
        # No arbitrary endpoints, streams, credentials, or order profiles can be supplied.
        for option in ("--endpoint-host", "--connection-settings", "--aggr-settings", "--profile-id"):
            result = subprocess.run([binary, option, "PROD"], capture_output=True)
            assert result.returncode == 1
        for seconds in ("0", "-1", "604801", "abc"):
            assert subprocess.run([binary, "--observation-seconds", seconds], capture_output=True).returncode == 1
        assert subprocess.run([binary, "--output", str(Path(root) / "unarmed.jsonl")],
                              capture_output=True).returncode == 1
    print("PASS: transactions, invalidation, four streams, CP1251, UTF-8, exclusive evidence, TEST allowlist")


if __name__ == "__main__":
    main()
