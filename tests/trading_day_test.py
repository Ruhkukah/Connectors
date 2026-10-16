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
    aggr_events = [row for row in rows if row["data"].get("stream") == "FORTS_AGGR20_REPL"]
    assert not any(row["event"] in {"transaction_begin", "transaction_commit", "replstate"} for row in aggr_events), \
        "AGGR transaction traffic still dominates the interaction journal"
    assert all(row["data"]["name"] == "sys_events" for row in aggr_events if row["event"] == "stream_row")
    assert any(row["event"] == "stream_row" and row["data"]["name"] == "sys_events" for row in aggr_events), \
        "exchange session announcements were dropped with the book traffic"
    assert len(aggr_events) < 100, "AGGR journal volume is not bounded to lifecycle and announcements"
    commands = [row["data"] for row in rows if row["event"] == "command"]
    assert {row["name"] for row in commands} >= {"AddOrder", "DelOrder", "MoveOrder", "DelUserOrders"}
    assert all("payload_hex" in row and "fields" in row for row in commands)
    output = subprocess.check_output([sys.executable, sys.argv[3], str(journal)], text=True)
    histories = {row["client_order_id"]: row["events"] for row in map(json.loads, output.splitlines())}
    mass_cancel_ids = {row["data"]["user_id"] for row in rows
                       if row["event"] == "command" and row["data"]["name"] == "DelUserOrders" and
                       not row["data"]["client_order_id"]}
    assert mass_cancel_ids, "native fixture did not exercise account-wide cancellation"
    mass_cancel_events = [{"utc": row["utc"], "event": row["event"], "data": row["data"]} for row in rows
                          if row["event"] in {"command", "command_result", "reply", "timeout"} and
                          row["data"]["user_id"] in mass_cancel_ids]
    assert histories["cancel-all"] == mass_cancel_events, \
        "account-wide command/reply/result history lost events or changed raw fields"
    assert all(event["data"]["client_order_id"] == "" for event in histories["cancel-all"]
               if event["event"] == "command"), "mass cancellation acquired a fabricated per-order ID"
    assert {"partial", "cancelled", "moved", "carry", "restart-working", "evening", "recovered:322:64001"} <= histories.keys()
    assert any(event["event"] == "trade" for event in histories["partial"])
    assert any(event["event"] == "reply" and event["data"].get("order_id1") == 62003 for event in histories["moved"])
    assert any(event["event"] == "order" and event["data"].get("order_id") == 64001 for event in histories["carry"])
    assert any(event["event"] == "order" and event["data"].get("state") == "Cancelled" for event in histories["recovered:322:64001"])
    assert sum(event["event"] == "command" and event["data"]["name"] == "DelUserOrders" for event in histories["lost-reply"]) == 3
    assert any(event["event"] == "order" and event["data"].get("state") == "Unknown" and
               event["data"].get("operator_action_required") for event in histories["lost-reply"])
