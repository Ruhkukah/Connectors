"""Cancellation retry causes stay correlated to every actual command."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

events = []
for uid, key in [(11, "owned"), (12, "owned"), (21, "")]:
    events.append({"event": "command", "data": {"user_id": uid, "client_order_id": key,
                                               "name": "DelOrder" if key else "DelUserOrders"}})
    events.append({"event": "timeout", "data": {"user_id": uid}})
    if uid != 12:
        events.append({"event": "transport_retry", "data": {"user_id": uid,
                                                             "client_order_id": key,
                                                             "message": "transport retry"}})
for event in events:
    event["utc"] = "2026-10-03T00:00:00.000000Z"
with tempfile.TemporaryDirectory() as directory:
    journal = Path(directory) / "journal.ndjson"
    journal.write_text("".join(json.dumps(row) + "\n" for row in events))
    rows = [json.loads(row) for row in subprocess.check_output(
        [sys.executable, sys.argv[1], str(journal)], text=True).splitlines()]
    histories = {row["client_order_id"]: row["events"] for row in rows}
    for key, uids in [("owned", {11, 12}), ("cancel-all", {21})]:
        expected = [{"utc": row["utc"], "event": row["event"], "data": row["data"]}
                    for row in events if row["data"]["user_id"] in uids]
        assert histories[key] == expected, "timeout/retry cause lost its command correlation"
