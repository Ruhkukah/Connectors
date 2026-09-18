#!/usr/bin/env python3
"""Streaming journal-v2 transaction validator; incomplete transactions are not evidence."""

import argparse
import hashlib
import json

SEED = 14695981039346656037
MASK = (1 << 64) - 1
MAX_RECORD = 1024 * 1024


def inspect(path):
    pending = {}
    result = dict(committed_transactions=0, committed_rows=0, discarded_rows=0,
                  incomplete_transactions=0, incomplete_rows=0, clean_shutdown=False,
                  clean_shutdown_marker=False,
                  torn_tail=False)
    artifact_hash = hashlib.sha256()
    sequence = 0

    def discard(key):
        tx = pending.pop(key, None)
        if tx is not None:
            result["incomplete_transactions"] += 1
            result["incomplete_rows"] += tx["rows"]

    with open(path, "rb") as source:
        while raw := source.readline(MAX_RECORD + 1):
            artifact_hash.update(raw)
            if len(raw) > MAX_RECORD:
                raise ValueError("journal record exceeds bound")
            if not raw.endswith(b"\n"):
                result["torn_tail"] = True
                result["clean_shutdown_marker"] = False
                break
            event = json.loads(raw.decode("utf-8"))
            sequence += 1
            if event["seq"] != sequence:
                raise ValueError("journal sequence gap")
            kind = event["kind"]
            result["clean_shutdown_marker"] = kind == "clean_shutdown"
            key = (event.get("generation"), event.get("stream"))
            if kind == "transaction_begin":
                discard(key)
                pending[key] = dict(id=event["transaction_id"], rows=0, hash=SEED, invalid=False)
            elif kind == "row":
                tx = pending.get(key)
                if tx is None or tx["id"] != event["transaction_id"]:
                    raise ValueError("row lacks matching transaction begin")
                tx["rows"] += 1
                for byte in raw:
                    tx["hash"] = ((tx["hash"] ^ byte) * 1099511628211) & MASK
            elif kind == "transaction_commit":
                tx = pending.pop(key, None)
                if not event["transaction_committed"]:
                    result["discarded_rows"] += 0 if tx is None else tx["rows"]
                    continue
                if tx is None or tx["id"] != event["transaction_id"] or tx["invalid"]:
                    raise ValueError("commit lacks a valid matching transaction")
                if (tx["rows"] != event["recorded_row_count"] or
                        str(tx["hash"]) != event["recorded_row_fnv1a64"] or
                        event["source_row_count"] < tx["rows"]):
                    raise ValueError("transaction row count/hash mismatch")
                result["committed_transactions"] += 1
                result["committed_rows"] += tx["rows"]
            elif kind == "clear_deleted":
                if key in pending:
                    pending[key]["invalid"] = True
            elif kind in ("lifenum", "close", "open", "callback_error"):
                discard(key)
            elif kind in ("generation_error", "generation_end"):
                for candidate in list(pending):
                    if candidate[0] == event["generation"]:
                        discard(candidate)
            if len(pending) > 4:
                raise ValueError("more than four simultaneous public-stream transactions")
    for key in list(pending):
        discard(key)
    result["clean_shutdown"] = (result["clean_shutdown_marker"] and not result["torn_tail"] and
                                result["incomplete_transactions"] == 0)
    result["sha256"] = artifact_hash.hexdigest()
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal")
    args = parser.parse_args()
    print(json.dumps(inspect(args.journal), sort_keys=True))
