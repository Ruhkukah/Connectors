#!/usr/bin/env python3
"""Explicit TEST-only one-lot DTC lifecycle. JSON plan/receipt, protobuf-v8 wire.

Validation is offline. Execution requires --allow-test-orders, a fresh external
five-minute writer gate and a private local DTC secret. The client never retries
an order, reconnects, sends a market order or performs account-wide cancellation.
An uncertain/rejected result stops the sequence; the owner must reconcile it.
"""
import argparse
import datetime as dt
from decimal import Decimal, InvalidOperation
import json
import math
import os
from pathlib import Path
import re
import socket
import stat
import struct
import time

EXCHANGE = "MOEX_SPECTRA"


class Uncertain(RuntimeError):
    pass


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def varint(value):
    require(type(value) is int and value >= 0, "unsigned protobuf integer required")
    out = bytearray()
    while value > 127:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def number(field, value):
    return varint(field << 3) + varint(value)


def text(field, value):
    raw = value.encode("utf-8")
    return varint((field << 3) | 2) + varint(len(raw)) + raw


def real(field, value):
    return varint((field << 3) | 1) + struct.pack("<d", float(value))


def decode(payload):
    fields, pos = {}, 0
    def take(n):
        nonlocal pos
        require(pos + n <= len(payload), "truncated protobuf")
        value = payload[pos:pos+n]
        pos += n
        return value
    def integer():
        value = 0
        for shift in range(0, 70, 7):
            b = take(1)[0]
            value |= (b & 127) << shift
            if b < 128:
                return value
        raise ValueError("invalid varint")
    while pos < len(payload):
        key = integer()
        field, wire = key >> 3, key & 7
        require(field > 0 and field not in fields, "invalid/duplicate protobuf field")
        if wire == 0:
            value = integer()
        elif wire == 1:
            value = struct.unpack("<d", take(8))[0]
        elif wire == 2:
            value = take(integer()).decode("utf-8")
        elif wire == 5:
            value = struct.unpack("<f", take(4))[0]
        else:
            raise ValueError("unsupported protobuf wire type")
        fields[field] = value
    return fields


def validate_plan(plan, now=None):
    now = time.time() if now is None else now
    require(plan.get("environment") == "TEST", "TEST environment required")
    require(plan.get("quantity") == 1 and type(plan["quantity"]) is int and
            plan.get("time_in_force") == "day", "one-lot Day limits only")
    require(plan.get("side") in ("buy", "sell"), "explicit side required")
    require(re.fullmatch(r"[A-Za-z0-9_-]{1,48}", plan.get("client_order_id", "")), "unique client ID required")
    require(re.fullmatch(r"[0-9a-f]{40}", plan.get("source_sha", "")), "source pin required")
    require(plan.get("instance_id") and plan.get("account") and plan.get("symbol") and
            type(plan.get("isin_id")) is int and plan["isin_id"] > 0, "account/instrument/instance required")
    gate = plan.get("writer_gate", {})
    require(gate.get("passed") is True and type(gate.get("samples")) is int and gate["samples"] >= 11 and
            type(gate.get("span_seconds")) in (int, float) and math.isfinite(gate["span_seconds"]) and gate["span_seconds"] >= 300,
            "external five-minute writer gate required")
    begin, end = plan.get("registered_ext_id_range", (0, 0))
    require(type(begin) is int and type(end) is int and 0 < begin <= end < 2147483647,
            "registered ext-ID range required; server owns ID allocation")
    observed = dt.datetime.fromisoformat(gate["observed_at"])
    deadline = dt.datetime.fromisoformat(plan["absolute_term"])
    require(observed.tzinfo is not None and deadline.tzinfo is not None and
            0 <= now-observed.timestamp() <= 10 and 0 < deadline.timestamp()-now <= 300,
            "fresh gate and bounded absolute deadline required")
    try:
        prices = {key: Decimal(str(plan[key])) for key in
                  ("add_price", "move_price", "best_bid", "best_ask", "min_step", "lower_price", "upper_price")}
    except (InvalidOperation, KeyError) as error:
        raise ValueError("exact decimal price/grid/bounds required") from error
    require(all(x.is_finite() and x > 0 for x in prices.values()) and
            prices["best_bid"] < prices["best_ask"], "finite two-sided committed quote required")
    for key in ("add_price", "move_price"):
        price = prices[key]
        require(price % prices["min_step"] == 0 and
                prices["lower_price"] < price < prices["upper_price"], "price outside tick/bounds")
        require(price <= prices["best_bid"] * Decimal("0.99") if plan["side"] == "buy" else
                price >= prices["best_ask"] * Decimal("1.01"), "price must remain at least1% passive")
    require(prices["move_price"] < prices["add_price"] if plan["side"] == "buy" else
            prices["move_price"] > prices["add_price"], "Move must go farther from touch")
    return deadline.timestamp()


def read_secret(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.geteuid() and
                stat.S_IMODE(info.st_mode) == 0o600, "owned mode0600 local DTC secret required")
        secret = stream.read(258).decode("utf-8").removesuffix("\n")
    require(1 <= len(secret.encode()) <= 256 and not any(c in secret for c in "\0\r\n"), "invalid DTC secret")
    return secret


class Client:
    def __init__(self, sock, deadline, record, timeout=2):
        self.sock, self.deadline, self.record, self.timeout = sock, deadline, record, timeout
        self.mutations = []
        self.last_order = None
        self.fresh_books = []

    def send(self, kind, payload=b""):
        if time.time() >= self.deadline:
            raise Uncertain("absolute deadline reached; nothing sent, no retry")
        try:
            self.sock.sendall(struct.pack("<HH", len(payload)+4, kind)+payload)
        except OSError as error:
            raise Uncertain("socket write uncertain; no retry") from error

    def receive(self, until):
        def exact(n):
            out = bytearray()
            while len(out) < n:
                left = min(until, self.deadline)-time.time()
                if left <= 0:
                    raise Uncertain("response deadline expired; no retry")
                self.sock.settimeout(left)
                try:
                    block = self.sock.recv(n-len(out))
                except OSError as error:
                    raise Uncertain("response uncertain; no retry") from error
                if not block:
                    raise Uncertain("peer closed; reconcile before another command")
                out.extend(block)
            return out
        size, kind = struct.unpack("<HH", exact(4))
        require(4 <= size <= 65535, "invalid DTC frame size")
        payload = bytes(exact(size-4))
        fields = {} if kind == 7 else decode(payload)
        self.record({"utc_ns": time.time_ns(), "type": kind, "fields": fields})
        if kind == 301 and (fields.get(19, 0) != 0 or fields.get(10) in (7, 10)):
            raise Uncertain("unexpected fill; no compensating order")
        if kind in (5, 103, 121, 302, 307, 509) or (kind == 116 and fields.get(2) == 1):
            raise Uncertain("DTC rejection/logoff; stop and reconcile")
        return kind, fields, payload

    def await_frame(self, kind, accept=lambda fields: True):
        until = min(self.deadline, time.time()+self.timeout)
        for _ in range(4096):
            received, fields, payload = self.receive(until)
            if received == kind and accept(fields):
                return fields, payload
        raise Uncertain("response frame bound exceeded")

    def initialize(self, plan, secret):
        self.send(6, struct.pack("<ii4s", 8, 4, b"DTC\0"))
        _, payload = self.await_frame(7)
        require(payload == struct.pack("<ii4s", 8, 4, b"DTC\0"), "protobuf-v8 negotiation required")
        self.send(1, number(1, 8)+text(2, "kairos")+text(3, secret)+number(7, 10)+text(9, "TEST lifecycle harness"))
        reply, _ = self.await_frame(2)
        require(reply.get(2) == 1 and reply.get(8) == 1 and reply.get(3) == "MOEX FOL TEST trading" and
                reply.get(6) == "MOEX live TEST DTC", "authenticated FOL TEST trading endpoint required")
        self.send(506, number(1, 1)+text(2, plan["symbol"])+text(3, EXCHANGE))
        reply, _ = self.await_frame(507)
        require(reply.get(2) == plan["symbol"] and reply.get(3) == EXCHANGE and
                reply.get(33) == plan["isin_id"], "current instrument definition mismatch")
        self.send(400, number(1, 2))
        reply, _ = self.await_frame(401)
        require(reply.get(1) == 1 and reply.get(3) == plan["account"], "account mismatch")
        self.reconcile(plan, 3)

    def reconcile(self, plan, request):
        self.send(300, number(1, request)+text(4, plan["account"]))
        reply, _ = self.await_frame(301, lambda x: x.get(1) == request)
        require(reply.get(28) == 1 and reply.get(26) == plan["account"], "working orders/account not empty")
        self.send(305, number(1, request+1)+text(2, plan["account"]))
        reply, _ = self.await_frame(306, lambda x: x.get(1) == request+1)
        require(reply.get(6) == 0 and reply.get(9) == plan["account"] and
                reply.get(4) == plan["symbol"] and reply.get(5) == EXCHANGE, "position/account not flat")
        return {"orders_request": request, "position_request": request+1, "orders_empty": True, "position": 0}

    def fresh_book(self, plan, price):
        # Snapshot action3 is supported by this endpoint; it does not subscribe
        # or require a second client/incremental-book implementation.
        self.send(102, number(1, 3)+number(2, 1)+text(3, plan["symbol"])+text(4, EXCHANGE)+number(5, 20))
        rows, identity, levels = [], None, set()
        until = min(self.deadline, time.time()+self.timeout)
        for _ in range(4096):
            kind, fields, _ = self.receive(until)
            if kind != 145:
                continue
            current = (fields.get(11), fields.get(12), fields.get(13), fields.get(15))
            if identity is None:
                identity = current
            marker, side, level = fields.get(7), fields.get(6), fields.get(5)
            require(identity == current and all(type(v) is int and v > 0 for v in current) and
                    fields.get(1) == 1 and side in (1, 2) and type(level) is int and 1 <= level <= 20 and
                    (side, level) not in levels and marker in ((1, 3) if not rows else (1, 2)),
                    "invalid/inconsistent DTC depth snapshot")
            quote, quantity = fields.get(2), fields.get(3)
            require(isinstance(quote, (int, float)) and math.isfinite(quote) and quote > 0 and
                    isinstance(quantity, (int, float)) and math.isfinite(quantity) and quantity > 0,
                    "invalid DTC depth price/quantity")
            rows.append((side, level, Decimal(str(quote))))
            levels.add((side, level))
            require(len(rows) <= 40 and len(rows) <= identity[3], "depth snapshot row bound exceeded")
            if marker == 1:
                require(len(rows) == identity[3], "incomplete DTC depth snapshot")
                break
        else:
            raise Uncertain("depth frame bound exceeded")
        best = {side: value for side, level, value in rows if level == 1}
        require(1 in best and 2 in best and best[1] < best[2], "fresh server book is not two-sided/uncrossed")
        require(not self.fresh_books or identity[0] == self.fresh_books[0]["epoch"], "source epoch changed; stop entry")
        require(Decimal(price) <= best[1]*Decimal("0.99") if plan["side"] == "buy" else
                Decimal(price) >= best[2]*Decimal("1.01"), "planned price is not1% passive to fresh server book")
        self.fresh_books.append({"received_utc_ns": time.time_ns(), "best_bid": str(best[1]),
            "best_ask": str(best[2]), "epoch": identity[0], "batch": identity[1], "version": identity[2],
            "rows": len(rows), "planned_price": price})

    def lifecycle(self, plan):
        cid, account = plan["client_order_id"], plan["account"]
        def mutation(kind, payload, state, price=None, server_id=None):
            if kind in (208, 204):
                validate_plan(plan)
                self.fresh_book(plan, price)
            self.mutations.append(kind)  # Includes uncertain writes; never resend.
            self.record({"utc_ns": time.time_ns(), "attempt_type": kind, "client_order_id": cid})
            self.send(kind, payload)
            def accepted(fields):
                if fields.get(8) != cid:
                    return False
                if fields.get(27) or fields.get(10) in (0, 9):
                    raise Uncertain("order rejected/unknown; no retry")
                if fields.get(10) != state:
                    return False
                require(fields.get(26) == account and fields.get(7) and fields.get(9), "order identity/account absent")
                require(server_id is None or fields[7] == server_id, "stable server order identity changed")
                if price is not None and Decimal(str(fields.get(14))) != Decimal(price):
                    return False
                require(fields.get(19) == 0 and fields.get(20) == (0 if state == 8 else 1), "fill/quantity not proven")
                return True
            reply = self.await_frame(301, accepted)[0]
            self.last_order = {"server_order_id": reply[7], "native_order_id": reply[9],
                               "state": reply[10], "remaining": reply[20], "filled": reply[19]}
            return reply
        body = text(1, plan["symbol"])+text(2, EXCHANGE)+text(3, account)+text(4, cid)
        body += number(5, 2)+number(6, 1 if plan["side"] == "buy" else 2)
        body += text(17, plan["add_price"])+real(9, 1)+number(10, 1)
        added = mutation(208, body, 4, plan["add_price"])
        server = added[7]
        moved = mutation(204, text(1, server)+text(2, cid)+real(5, 1)+number(6, 1)+
                         text(12, account)+text(13, plan["move_price"]), 4, plan["move_price"], server)
        require(moved[9] != added[9], "native replacement ID did not change")
        cancelled = mutation(203, text(1, server)+text(2, cid)+text(3, account), 8, server_id=server)
        post_cancel = self.reconcile(plan, 5)
        return {"client_order_id": cid, "server_order_id": server,
                "native_order_ids": [added[9], moved[9]], "cancelled": cancelled[10] == 8,
                "mutation_attempts": self.mutations, "zero_fills": True,
                "fresh_server_books": self.fresh_books, "post_cancel_protocol_reconciliation": post_cancel,
                "covering_userorderbook": "external controller proof still required"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", required=True, type=Path)
    parser.add_argument("--validate", action="store_true", help="Pure offline plan validation; never opens a socket")
    parser.add_argument("--allow-test-orders", action="store_true")
    parser.add_argument("--secret-file", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--port", type=int, default=11300)
    args = parser.parse_args()
    plan = json.loads(args.plan.read_text())
    deadline = validate_plan(plan)
    if args.validate:
        print(json.dumps({"valid": True, "connected": False, "orders_sent": 0}))
        return 0
    require(args.allow_test_orders and args.secret_file and args.output_dir, "explicit TEST-order opt-in, secret and output directory required")
    os.umask(0o077)
    args.output_dir.mkdir(mode=0o700, exist_ok=False)
    secret = read_secret(args.secret_file)
    result = {"external_writer_gate_claim": {"source_sha": plan["source_sha"], "instance_id": plan["instance_id"],
              "registered_ext_id_range": plan["registered_ext_id_range"]}, "server_source_pin_verified": False,
              "environment": "TEST", "status": "uncertain", "retry": False}
    sock = client = None
    with (args.output_dir/"events.jsonl").open("x") as events:
        def record(value):
            events.write(json.dumps(value, separators=(",", ":"), allow_nan=False).replace(secret, "<redacted>")+"\n")
            events.flush()
        try:
            sock = socket.create_connection(("127.0.0.1", args.port), timeout=2)
            client = Client(sock, deadline, record)
            client.initialize(plan, secret)
            validate_plan(plan)  # Do not submit on a gate that expired during setup.
            result.update(client.lifecycle(plan), status="cancelled")
        except (OSError, ValueError, Uncertain) as error:
            result["error"] = str(error).replace(secret, "<redacted>")
            result["mutation_attempts"] = client.mutations if client else []
            result["last_confirmed_order"] = client.last_order if client else None
            result["fresh_server_books"] = client.fresh_books if client else []
            result["reconciliation_required"] = bool(client and client.mutations)
        finally:
            if sock is not None:
                sock.close()
    (args.output_dir/"receipt.json").write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result))
    return 0 if result["status"] == "cancelled" else 2


if __name__ == "__main__":
    raise SystemExit(main())
