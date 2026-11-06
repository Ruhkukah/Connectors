#!/usr/bin/env python3
"""Offline check of saved FOL writer status; never connects or sends commands."""
import argparse
import json
from pathlib import Path

REQUIRED_STREAMS = {
    "FORTS_TRADE_REPL", "FORTS_USERORDERBOOK_REPL", "FORTS_POS_REPL",
    "FORTS_PART_REPL", "FORTS_REFDATA_REPL", "FORTS_SESSIONSTATE_REPL",
    "FORTS_INSTRUMENTSTATE_REPL", "FORTS_ORDLOG_REPL", "p2mqreply",
}
TERMINAL = {"Cancelled", "Rejected", "Filled"}


def check_status(status, isin_id, *, require_flat=True, sole_instance=False):
    """Return refusal reasons; native admission and the owner's authority still apply."""
    errors = []

    def need(condition, reason):
        if not condition:
            errors.append(reason)

    need(status.get("reconstructing") is False, "private reconstruction incomplete")
    need(not status.get("log_error") and status.get("journal_failed") is False,
         "journal not healthy")
    need(status.get("operator_action_required") is False and status.get("queued") == 0,
         "unresolved or queued operation")
    config = status.get("configuration", {})
    need(config.get("allow_orders") is True, "writer is not order enabled")
    need(config.get("sole_instance") is sole_instance, "unexpected sole-instance mode")
    need(status.get("market_data_source") == "full_order_log", "source is not FOL")
    transport = status.get("transport", {})
    need(transport.get("valid") is True, "native transport not valid")
    streams = {row.get("stream"): row for row in transport.get("listeners", [])}
    for name in sorted(REQUIRED_STREAMS):
        row = streams.get(name, {})
        need(row.get("online") is True and row.get("snapshot_complete") is True
             and row.get("native", {}).get("state") == 3
             and row.get("error", {}).get("code") == 0, "stream not ready: " + name)
    anchor = status.get("pos_anchor") or {}
    trade = status.get("trade_replay") or {}
    need(anchor.get("online") is True and anchor.get("snapshot_complete") is True
         and anchor.get("trades_lifenum", 0) > 0
         and trade.get("anchor_ready") is True
         and trade.get("accepted_lifenum") == anchor.get("trades_lifenum"),
         "current POS-anchored TRADE not ready")
    # order_book_startup_barrier_ready compares the latest periodic USERORDERBOOK
    # revision with the original bootstrap cutoff. It can become false after a
    # newer publication during normal delta processing. Completed reconstruction
    # plus current stream/anchor health above is the live prerequisite.
    instrument = next((row for row in status.get("instruments", [])
                       if row.get("isin_id") == isin_id), {})
    book = instrument.get("book") or {}
    terms = instrument.get("session_terms") or {}
    session = book.get("source_session_id", 0)
    need(session > 0 and session == terms.get("sess_id") == status.get("sess_id"),
         "FOL/current session authority differs or is unknown")
    need(book.get("valid") is True and book.get("crossed") is False
         and instrument.get("order_entry_ready") is True, "native FOL entry gate refused")
    bid, ask = book.get("best_bid") or {}, book.get("best_ask") or {}
    need(0 < bid.get("price_scaled", 0) < ask.get("price_scaled", 0)
         and bid.get("quantity", 0) > 0 and ask.get("quantity", 0) > 0,
         "valid two-sided committed book unavailable")
    need(terms.get("interval_valid") is True and terms.get("min_step_scaled", 0) > 0,
         "current price bounds/grid unavailable")
    positions = status.get("positions")
    orders = status.get("orders")
    need(isinstance(positions, list) and all(row.get("xpos") == 0 for row in positions),
         "account flat position unproven")
    need(isinstance(orders, list), "account order inventory unavailable")
    if require_flat and isinstance(orders, list):
        need(all(row.get("state") in TERMINAL for row in orders), "working orders remain")
    return errors


def check_gate(samples):
    if not isinstance(samples, list) or len(samples) < 7:
        return ["at least seven gate samples required"]
    elapsed = [row.get("elapsed_seconds", -1) for row in samples]
    if elapsed[-1] < 180 or any(b - a < 29.5 for a, b in zip(elapsed, elapsed[1:])):
        return ["three-minute gate/sample spacing incomplete"]
    return []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--status", type=Path, required=True)
    parser.add_argument("--isin", type=int, required=True)
    parser.add_argument("--gate", type=Path)
    parser.add_argument("--sole-instance", action="store_true")
    args = parser.parse_args()
    status = json.loads(args.status.read_text())
    status = status.get("status", status.get("response", status))
    errors = check_status(status, args.isin, sole_instance=args.sole_instance)
    if args.gate:
        gate = json.loads(args.gate.read_text())
        samples = gate.get("samples") if isinstance(gate, dict) else gate
        errors += check_gate(samples)
    print(json.dumps({"ready": not errors, "refusals": errors}))
    return 0 if not errors else 2


if __name__ == "__main__":
    raise SystemExit(main())
