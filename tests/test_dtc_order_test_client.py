"""In-memory fake server exercises the exact DTC wire; no sockets/native calls."""
import datetime as dt
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("dtc_client", Path(__file__).parents[1]/"scripts/dtc_order_test_client.py")
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)


def plan():
    now = dt.datetime.now(dt.timezone.utc)
    return {"environment": "TEST", "source_sha": "a"*40, "instance_id": "unit-fixture",
            "registered_ext_id_range": [1000, 1999], "account": "FAKEACCOUNT", "symbol": "FAKE-12.26",
            "isin_id": 1001, "client_order_id": "unit-one", "side": "buy", "quantity": 1,
            "time_in_force": "day", "add_price": "9.8", "move_price": "9.7", "best_bid": "10",
            "best_ask": "10.1", "min_step": "0.1", "lower_price": "9", "upper_price": "11",
            "writer_gate": {"passed": True, "samples": 11, "span_seconds": 301,
                            "observed_at": now.isoformat()},
            "absolute_term": (now+dt.timedelta(seconds=60)).isoformat()}


class FakeServer:
    """Only protocol packets enter this server, never the client's plan object."""
    def __init__(self, failure=None):
        self.output = bytearray()
        self.mutations = []
        self.failure = failure
        self.current = None
        self.snapshot_requests = 0
        self.reconciliations = []

    def settimeout(self, _):
        pass

    def close(self):
        self.closed = True

    def queue(self, kind, payload):
        self.output.extend(struct.pack("<HH", len(payload)+4, kind)+payload)

    def order(self, state, price="9.8", native="native-one", error="", filled=0):
        body = c.text(7, "server-one")+c.text(8, "unit-one")+c.text(9, native)+c.number(10, state)
        body += c.number(11, 3)+c.real(14, price)+c.real(18, 1)+c.real(19, filled)
        body += c.real(20, 0 if state in (8, 9) else 1)+c.text(26, "FAKEACCOUNT")
        if error:
            body += c.text(27, error)
        self.queue(301, body)

    def sendall(self, packet):
        size, kind = struct.unpack("<HH", packet[:4])
        assert size == len(packet)
        if kind == 6:
            assert packet[4:] == struct.pack("<ii4s", 8, 4, b"DTC\0")
            return self.queue(7, packet[4:])
        f = c.decode(packet[4:])
        if kind == 1:
            assert f[1] == 8 and f[2] == "kairos" and f[3] == "unit-secret"
            self.queue(2, c.number(2, 1)+c.number(8, 1)+c.text(3, "PROD" if self.failure == "wrong-endpoint" else "MOEX FOL TEST trading")+c.text(6, "MOEX live TEST DTC"))
        elif kind == 506:
            self.queue(507, c.text(2, "WRONG" if self.failure == "definition" else "FAKE-12.26")+
                       c.text(3, c.EXCHANGE)+c.number(33, 1001))
        elif kind == 400:
            self.queue(401, c.number(1, 1)+c.text(3, "FAKEACCOUNT"))
        elif kind == 300:
            self.reconciliations.append(f[1])
            if self.failure == "working" or (self.failure == "post-cancel-working" and f[1] == 5):
                self.queue(301, c.number(1, f[1])+c.number(28, 0))
            else:
                self.queue(301, c.number(1, f[1])+c.number(28, 1)+c.text(26, "FAKEACCOUNT"))
        elif kind == 305:
            self.reconciliations.append(f[1])
            position = 1 if self.failure == "post-cancel-position" and f[1] == 6 else 0
            self.queue(306, c.number(1, f[1])+c.real(6, position)+c.text(9, "FAKEACCOUNT")+
                       c.text(4, "FAKE-12.26")+c.text(5, c.EXCHANGE))
        elif kind == 102:
            assert f == {1: 3, 2: 1, 3: "FAKE-12.26", 4: c.EXCHANGE, 5: 20}
            self.snapshot_requests += 1
            if self.failure == "fill-during-snapshot" and self.snapshot_requests == 2:
                self.order(10, filled=1)
            bid = 9.7 if self.failure == "quote-before-add" or (self.failure == "quote-before-move" and self.snapshot_requests == 2) else 10
            ask = 9.5 if self.failure == "crossed-book" else 10.1
            for n, (side, value) in enumerate(((1, bid), (2, ask))):
                symbol = 2 if self.failure == "wrong-book-id" else 1
                body = c.number(1, symbol)+c.real(2, value)+c.real(3, 10)+c.number(5, 1)+c.number(6, side)
                body += c.number(7, 3 if n == 0 else 1)+c.number(11, 1)+c.number(12, self.snapshot_requests)
                body += c.number(13, 7)+c.number(15, 3 if self.failure == "incomplete-book" else 2)
                self.queue(145, body)
        elif kind == 208:
            self.mutations.append(kind)
            assert f[1] == "FAKE-12.26" and f[2] == c.EXCHANGE and f[3] == "FAKEACCOUNT"
            assert f[4] == "unit-one" and f[5] == 2 and f[6] == 1 and f[9] == 1 and f[10] == 1
            assert f[17] == "9.8" and 7 not in f  # Exact decimal text, never rounded float price.
            if self.failure == "reject":
                self.order(9, error="native rejection")
            elif self.failure == "fill":
                self.order(10, filled=1)
            elif self.failure == "add-timeout":
                pass
            else:
                self.order(4)
        elif kind == 204:
            self.mutations.append(kind)
            assert f[1] == "server-one" and f[2] == "unit-one" and f[5] == 1 and f[6] == 1
            assert f[12] == "FAKEACCOUNT" and f[13] == "9.7"
            if self.failure == "move-timeout":
                pass
            elif self.failure == "move-reject":
                self.order(4, error="move refused")
            else:
                self.order(4, "9.7", "native-two")
        elif kind == 203:
            self.mutations.append(kind)
            assert f == {1: "server-one", 2: "unit-one", 3: "FAKEACCOUNT"}
            self.order(8, "9.7", "native-two")
        else:
            raise AssertionError("unexpected request " + str(kind))

    def recv(self, length):
        if not self.output:
            raise socket.timeout("fake silence")
        count = min(length, 3, len(self.output))  # Deliberately fragmented frames.
        out = bytes(self.output[:count])
        del self.output[:count]
        return out


class ClientTests(unittest.TestCase):
    def run_client(self, failure=None):
        p = plan()
        server, records = FakeServer(failure), []
        client = c.Client(server, c.validate_plan(p), records.append)
        client.initialize(p, "unit-secret")
        return server, records, client, p

    def test_add_move_cancel_wire_and_confirmations(self):
        server, records, client, p = self.run_client()
        result = client.lifecycle(p)
        self.assertEqual(server.mutations, [208, 204, 203])
        self.assertEqual(result["native_order_ids"], ["native-one", "native-two"])
        self.assertTrue(result["cancelled"] and result["zero_fills"])
        self.assertEqual(server.snapshot_requests, 2)
        self.assertEqual(server.reconciliations, [3, 4, 5, 6])
        self.assertTrue(result["post_cancel_protocol_reconciliation"]["orders_empty"])
        self.assertNotIn("unit-secret", json.dumps(records))

    def test_rejected_add_stops_without_retry_or_more_commands(self):
        server, _, client, p = self.run_client("reject")
        with self.assertRaises(c.Uncertain):
            client.lifecycle(p)
        self.assertEqual(server.mutations, [208])

    def test_uncertain_add_is_not_resent(self):
        server, _, client, p = self.run_client("add-timeout")
        with self.assertRaises(c.Uncertain):
            client.lifecycle(p)
        self.assertEqual(server.mutations, [208])

    def test_uncertain_or_rejected_move_stops_for_reconciliation(self):
        for failure in ("move-timeout", "move-reject"):
            server, _, client, p = self.run_client(failure)
            with self.assertRaises(c.Uncertain):
                client.lifecycle(p)
            self.assertEqual(server.mutations, [208, 204])

    def test_unexpected_fill_stops_without_hedging(self):
        server, _, client, p = self.run_client("fill")
        with self.assertRaises(c.Uncertain):
            client.lifecycle(p)
        self.assertEqual(server.mutations, [208])

    def test_wrong_definition_or_working_orders_prevent_add(self):
        for failure in ("definition", "working", "wrong-endpoint"):
            server = FakeServer(failure)
            client = c.Client(server, time.time()+60, lambda _: None)
            with self.assertRaises(ValueError):
                client.initialize(plan(), "unit-secret")
            self.assertEqual(server.mutations, [])

    def test_fresh_server_quote_is_required_before_each_entry(self):
        for failure, attempts in (("quote-before-add", []), ("quote-before-move", [208])):
            server, _, client, p = self.run_client(failure)
            with self.assertRaises(ValueError):
                client.lifecycle(p)
            self.assertEqual(server.mutations, attempts)

    def test_fill_during_pre_move_snapshot_stops_before_move(self):
        server, _, client, p = self.run_client("fill-during-snapshot")
        with self.assertRaisesRegex(c.Uncertain, "unexpected fill"):
            client.lifecycle(p)
        self.assertEqual(server.snapshot_requests, 2)
        self.assertEqual(server.mutations, [208])

    def test_invalid_or_incomplete_fresh_book_prevents_add(self):
        for failure in ("crossed-book", "wrong-book-id", "incomplete-book"):
            server, _, client, p = self.run_client(failure)
            with self.assertRaises(ValueError):
                client.lifecycle(p)
            self.assertEqual(server.mutations, [])

    def test_post_cancel_working_order_or_position_is_not_flat_success(self):
        for failure in ("post-cancel-working", "post-cancel-position"):
            server, _, client, p = self.run_client(failure)
            with self.assertRaises(ValueError):
                client.lifecycle(p)
            self.assertEqual(server.mutations, [208, 204, 203])

    def test_test_day_one_lot_and_registration_required(self):
        for key, value in (("environment", "PROD"), ("quantity", 2), ("quantity", True),
                           ("time_in_force", "ioc"), ("registered_ext_id_range", [0, 1]),
                           ("source_sha", "unreviewed")):
            p = plan()
            p[key] = value
            with self.assertRaises(ValueError):
                c.validate_plan(p)

    def test_fresh_five_minute_gate_and_deadline_required(self):
        for key, value in (("passed", False), ("samples", 10), ("span_seconds", 299), ("span_seconds", float("inf")),
                           ("observed_at", "2020-01-01T00:00:00+00:00")):
            p = plan()
            p["writer_gate"][key] = value
            with self.assertRaises(ValueError):
                c.validate_plan(p)
        p = plan()
        p["absolute_term"] = "2020-01-01T00:00:00+00:00"
        with self.assertRaises(ValueError):
            c.validate_plan(p)

    def test_passive_tick_and_band_checks(self):
        for key, value in (("add_price", "10.1"), ("add_price", "9.85"),
                           ("move_price", "9.9"), ("add_price", "NaN"), ("lower_price", "9.8")):
            p = plan()
            p[key] = value
            with self.assertRaises(ValueError):
                c.validate_plan(p)

    def test_validate_cli_is_offline_and_needs_no_secret(self):
        with tempfile.TemporaryDirectory() as temp:
            file = Path(temp)/"plan.json"
            file.write_text(json.dumps(plan()))
            with patch.object(sys, "argv", ["client", "--plan", str(file), "--validate"]), \
                 patch.object(c.socket, "create_connection", side_effect=AssertionError("network forbidden")):
                self.assertEqual(c.main(), 0)

    def test_expired_absolute_deadline_sends_nothing(self):
        server = FakeServer()
        client = c.Client(server, time.time()-1, lambda _: None)
        with self.assertRaises(c.Uncertain):
            client.send(208, b"")
        self.assertEqual(server.mutations, [])

    def test_secret_requires_owned_private_regular_file(self):
        with tempfile.TemporaryDirectory() as temp:
            file = Path(temp)/"secret"
            file.write_text("unit-secret\n")
            file.chmod(0o600)
            self.assertEqual(c.read_secret(file), "unit-secret")
            file.chmod(0o644)
            with self.assertRaises(ValueError):
                c.read_secret(file)
            file.chmod(0o600)
            link = Path(temp)/"link"
            link.symlink_to(file)
            with self.assertRaises(OSError):
                c.read_secret(link)

    def test_cli_records_success_or_uncertainty_without_retries(self):
        for failure, code in ((None, 0), ("move-timeout", 2)):
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                file, secret, output = root/"plan.json", root/"secret", root/"new-output"
                file.write_text(json.dumps(plan()))
                secret.write_text("unit-secret\n")
                secret.chmod(0o600)
                server = FakeServer(failure)
                argv = ["client", "--plan", str(file), "--allow-test-orders", "--secret-file", str(secret),
                        "--output-dir", str(output)]
                with patch.object(sys, "argv", argv), patch.object(c.socket, "create_connection", return_value=server):
                    self.assertEqual(c.main(), code)
                result = json.loads((output/"receipt.json").read_text())
                self.assertTrue(server.closed)
                self.assertNotIn("unit-secret", (output/"events.jsonl").read_text())
                self.assertEqual(server.mutations, [208, 204, 203] if failure is None else [208, 204])
                if failure:
                    self.assertTrue(result["reconciliation_required"])
                    self.assertEqual(result["last_confirmed_order"]["native_order_id"], "native-one")

    def test_missing_owner_opt_in_does_not_connect(self):
        with tempfile.TemporaryDirectory() as temp:
            file = Path(temp)/"plan.json"
            file.write_text(json.dumps(plan()))
            with patch.object(sys, "argv", ["client", "--plan", str(file)]), \
                 patch.object(c.socket, "create_connection", side_effect=AssertionError("network forbidden")):
                with self.assertRaises(ValueError):
                    c.main()


if __name__ == "__main__":
    unittest.main()
