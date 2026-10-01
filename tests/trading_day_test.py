import json
import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="moex-native-day-") as directory:
    journal = pathlib.Path(directory) / "day.ndjson"
    subprocess.run([sys.argv[1], sys.argv[2], str(journal)], check=True)
    rows = [json.loads(line) for line in journal.read_text().splitlines()]
    assert all("utc" in row and "+03:00" in row["msk"] for row in rows)
    assert not any(row["event"] == "stream_row" and row["data"]["name"] == "orders_aggr" for row in rows)
    commands = [row["data"] for row in rows if row["event"] == "command"]
    assert {row["name"] for row in commands} >= {"AddOrder", "DelOrder", "MoveOrder", "DelUserOrders"}
    assert all("payload_hex" in row and "fields" in row for row in commands)
    output = subprocess.check_output([sys.executable, sys.argv[3], str(journal)], text=True)
    histories = {row["client_order_id"]: row["events"] for row in map(json.loads, output.splitlines())}
    assert {"partial", "cancelled", "moved", "carry", "restart-working", "evening", "recovered:322:64001"} <= histories.keys()
    assert any(event["event"] == "trade" for event in histories["partial"])
    assert any(event["event"] == "reply" and event["data"].get("order_id1") == 62003 for event in histories["moved"])
    assert any(event["event"] == "order" and event["data"].get("order_id") == 64001 for event in histories["carry"])
    assert any(event["event"] == "order" and event["data"].get("state") == "Cancelled" for event in histories["recovered:322:64001"])
