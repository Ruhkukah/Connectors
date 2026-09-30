#!/usr/bin/env python3
"""Print each client order's command/reply/replication history from the application log."""
import argparse
import json
from collections import defaultdict

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("journal")
args = parser.parse_args()
orders = defaultdict(list)
commands = {}
with open(args.journal, encoding="utf-8") as journal:
    for number, line in enumerate(journal, 1):
        event = json.loads(line)
        kind, data = event["event"], event["data"]
        key = data.get("client_order_id")
        if kind == "command":
            commands[data["user_id"]] = key or "cancel-all"
        if kind in {"reply", "timeout", "command_result"}:
            key = commands.get(data["user_id"])
        if key and kind in {"order", "trade", "command", "reply", "timeout", "command_result"}:
            orders[key].append({"utc": event["utc"], "event": kind, "data": data})
for key, events in orders.items():
    print(json.dumps({"client_order_id": key, "events": events}, ensure_ascii=False))
