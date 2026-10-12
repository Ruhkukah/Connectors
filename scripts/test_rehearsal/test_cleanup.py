"""Hermetic qualification cleanup checks: fake CLI/status, no CGate/network."""
from pathlib import Path
import copy
import importlib.util
import json
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("ops", str(Path(__file__).with_name('ops.py')))
ops = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ops)


class Clock:
    def __init__(self):
        self.now = ops.START + 100

    def sleep(self, seconds):
        self.now += max(float(seconds), .01)


class FakeProduct:
    def __init__(self, work, clock, outcome="filled", queued_polls=0):
        self.work, self.clock = Path(work), clock
        self.binary, self.env = "/fake/moexctl", {}
        self.isolation = None
        self.targets = [dict(symbol="CNY-12.26", isin_id=1)]
        self.outcome, self.queued_polls = outcome, queued_polls
        self.requests, self.cli_requests = [], []
        self.cid = None
        self.post_queries = 0
        self.official_confirmed = False
        self.settled = False
        self.kill = True
        self.trade_written = False
        (self.work / "interaction.ndjson").touch()
        self.before = dict(isin_id=1, xpos=1, buys=1, sells=0, last_deal_id=900)
        self.base = dict(
            queued=0, reconstructing=False, sess_id=12, operator_action_required=False,
            journal_failed=False, cancel_only=False, log_error="", cgate_key_check_failed=False,
            transport=dict(valid=True, connection=dict(state=3), publisher=dict(state=3),
                           listeners=[dict(stream=name, online=True, snapshot_complete=True,
                                           native=dict(state=3), error=dict(code=0))
                                      for name in sorted(ops.REQUIRED)]),
            trade_replay=dict(anchor_ready=True, accepted_lifenum=7,
                              used_anchor=dict(trades_lifenum=7)),
            pos_anchor=dict(online=True, snapshot_complete=True, trades_lifenum=7),
            configuration=dict(risk=dict(instruments=[dict(isin_id=1, max_quote_notional_scaled=2000000)])),
            instruments=[dict(isin_id=1, symbol="CNY-12.26", order_entry_ready=True,
                              session_terms=dict(sess_id=12, refdata_lifenum=7, interval_valid=True,
                                                 reference_price_scaled=1000000, min_step_scaled=1000,
                                                 lower_price_scaled=900000, upper_price_scaled=1100000),
                              book=dict(valid=True, best_bid=dict(price_scaled=1000000, quantity=2),
                                        best_ask=dict(price_scaled=1001000, quantity=2)))],
            positions=[copy.deepcopy(self.before)], orders=[])

    def command(self, line, permit_risk=True):
        self.requests.append(line)
        if line == "kill on":
            self.kill = True
        elif line.startswith("cancel "):
            client = line.split()[1]
            for row in self.base["orders"]:
                if row["client_order_id"] == client:
                    row.update(state="Cancelled", remaining=0)
        return dict(ok=True)

    def row(self):
        return dict(client_order_id=self.cid, instance_owned=True, operator_action_required=False,
                    state="Filled" if self.settled else "Working" if self.official_confirmed else "PendingNew",
                    order_id=111 if self.official_confirmed else 0, isin_id=1, side="sell", quantity=1,
                    executed=1 if self.settled else 0, remaining=0 if self.settled else 1)

    def write_trade(self):
        if self.trade_written or self.outcome == "no_credit":
            return
        self.trade_written = True
        trade = dict(event="trade", data=dict(client_order_id=self.cid, order_id=111, deal_id=901, quantity=1))
        with (self.work / "interaction.ndjson").open("a") as source:
            source.write(json.dumps(trade) + "\n")
            if self.outcome == "duplicate_credit":
                source.write(json.dumps(trade) + "\n")

    def status(self):
        value = copy.deepcopy(self.base)
        if self.cid:
            self.settled = self.official_confirmed and self.kill
            value["orders"] = [self.row()]
            if self.settled:
                self.write_trade()
                if self.outcome != "lagging_pos":
                    value["positions"][0]["xpos"] = 0
                if self.outcome != "missing_counter":
                    value["positions"][0].update(sells=1, last_deal_id=901)
        return value

    def cli(self, arguments, **kwargs):
        self.assert_cli(arguments)
        line = arguments[-1]
        self.cli_requests.append(dict(line=line, time=self.clock.now, confirmed=self.official_confirmed))
        if line == "kill off":
            self.kill = False
            if self.outcome == "uncertain_kill_off":
                return subprocess.CompletedProcess(arguments, 2, b'{"ok":false}', b"uncertain")
            result = dict(ok=True)
        elif line.startswith("place "):
            self.cid = line.split()[1]
            if self.outcome == "uncertain_place":
                return subprocess.CompletedProcess(arguments, 2, b'{"ok":false}', b"uncertain")
            result = dict(ok=True)
        elif line == "status":
            self.post_queries += 1
            self.official_confirmed = self.post_queries > self.queued_polls
            result = self.status()
        elif line == "kill on":
            self.kill = True
            result = dict(ok=True)
        else:
            raise AssertionError("unexpected cleanup CLI command: " + line)
        return subprocess.CompletedProcess(arguments, 0, json.dumps(result).encode(), b"")

    def assert_cli(self, arguments):
        if arguments[:3] != [self.binary, "plaza2", "cmd"]:
            raise AssertionError("cleanup bypassed the real product CLI boundary")
        if arguments[3:5] != ["--command-socket", str(self.work / "cmd.sock")]:
            raise AssertionError("cleanup used an unrelated socket")


class CleanupTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = Path(self.tmp.name)
        self.clock = Clock()

    def tearDown(self):
        self.tmp.cleanup()

    def run_cleanup(self, product):
        with patch.object(ops.time, "time", side_effect=lambda: self.clock.now), \
             patch.object(ops.time, "sleep", side_effect=self.clock.sleep), \
             patch.object(ops.subprocess, "run", side_effect=product.cli):
            return ops.clean_product(product, until=self.clock.now + 8)

    def test_uncertain_kill_off_always_restores_kill_and_never_places(self):
        product = FakeProduct(self.work, self.clock, "uncertain_kill_off")
        with self.assertRaises(ops.Stop):
            self.run_cleanup(product)
        self.assertTrue(product.kill)
        self.assertEqual([x["line"] for x in product.cli_requests], ["kill off", "kill on"])

    def test_uncertain_place_restores_kill_and_does_not_retry(self):
        product = FakeProduct(self.work, self.clock, "uncertain_place")
        with self.assertRaises(ops.Stop):
            self.run_cleanup(product)
        self.assertTrue(product.kill)
        self.assertEqual(sum(x["line"].startswith("place ") for x in product.cli_requests), 1)
        self.assertEqual(product.cli_requests[-1]["line"], "kill on")

    def test_queued_add_waits_official_identity_before_restoring_kill(self):
        product = FakeProduct(self.work, self.clock, queued_polls=3)
        result = self.run_cleanup(product)
        self.assertTrue(result["flat"])
        self.assertEqual(product.post_queries, 4)
        kill_on = next(x for x in product.cli_requests if x["line"] == "kill on")
        self.assertTrue(kill_on["confirmed"])
        self.assertGreater(kill_on["time"], product.cli_requests[1]["time"])

    def test_unique_credited_fill_and_pos_proof_allow_flat_completion(self):
        product = FakeProduct(self.work, self.clock)
        result = self.run_cleanup(product)
        self.assertTrue(result["cancel_confirmed"])
        self.assertTrue(result["flat"])
        self.assertTrue(product.kill)
        self.assertEqual(sum(x["line"].startswith("place ") for x in product.cli_requests), 1)

    def test_closed_session_flat_cleanup_does_not_repeat_mass_cancel(self):
        product = FakeProduct(self.work, self.clock)
        product.base['positions'][0]['xpos'] = 0
        product.base['instruments'][0]['order_entry_ready'] = False
        result = self.run_cleanup(product)
        self.assertTrue(result['cancel_confirmed'] and result['flat'])
        self.assertFalse(any(line.startswith('cancel-all ') for line in product.requests))
        self.assertEqual(product.cli_requests, [])

    def test_duplicate_credit_never_authorizes_second_ioc(self):
        product = FakeProduct(self.work, self.clock, "duplicate_credit")
        with self.assertRaisesRegex(ops.Stop, "cleanup_fill_position_unconfirmed"):
            self.run_cleanup(product)
        self.assertTrue(product.kill)
        self.assertEqual(sum(x["line"].startswith("place ") for x in product.cli_requests), 1)

    def test_missing_credit_never_authorizes_second_ioc(self):
        product = FakeProduct(self.work, self.clock, "no_credit")
        with self.assertRaisesRegex(ops.Stop, "cleanup_fill_position_unconfirmed"):
            self.run_cleanup(product)
        self.assertEqual(sum(x["line"].startswith("place ") for x in product.cli_requests), 1)

    def test_pos_counter_not_caught_up_never_authorizes_second_ioc(self):
        product = FakeProduct(self.work, self.clock, "missing_counter")
        with self.assertRaisesRegex(ops.Stop, "cleanup_fill_position_unconfirmed"):
            self.run_cleanup(product)
        self.assertEqual(sum(x["line"].startswith("place ") for x in product.cli_requests), 1)

    def test_nonflat_pos_never_produces_flat_result(self):
        product = FakeProduct(self.work, self.clock, "lagging_pos")
        with self.assertRaisesRegex(ops.Stop, "cleanup_fill_position_unconfirmed"):
            self.run_cleanup(product)
        self.assertFalse(ops.flat(product.status()))
        self.assertTrue(product.kill)
        self.assertFalse((self.work / "end-flat-private.json").exists())

    def test_isolation_loss_cancels_only_known_owned_and_refuses_ioc(self):
        product = FakeProduct(self.work, self.clock)
        product.base["orders"] = [
            dict(client_order_id="q-owned", instance_owned=True, order_id=301,
                 state="Working", remaining=1, operator_action_required=False),
            dict(client_order_id="q-foreign", instance_owned=False, order_id=302,
                 state="Rejected", remaining=0, operator_action_required=False),
        ]
        def lost():
            raise ops.Stop("router_client_isolation_changed")
        product.isolation = lost
        with self.assertRaisesRegex(ops.Stop, "router_client_isolation_changed"):
            self.run_cleanup(product)
        self.assertTrue(product.kill)
        self.assertIn("cancel q-owned", product.requests)
        self.assertNotIn("cancel q-foreign", product.requests)
        self.assertFalse(any(line.startswith("cancel-all ") for line in product.requests))
        self.assertEqual(product.cli_requests, [])


if __name__ == "__main__":
    unittest.main()
