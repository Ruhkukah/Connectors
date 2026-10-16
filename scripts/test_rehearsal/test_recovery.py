import copy
import unittest
import recovery


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.clock = 0.0
        self.calls = []
        self.before = dict(reconstructing=False, queued=0, orders=[], positions=[
            dict(isin_id=1, xpos=0, buys=0, sells=0, last_deal_id=0)])

    def run_recovery(self, states, elapsed=0):
        def status():
            self.calls.append("status")
            return copy.deepcopy(states.pop(0) if len(states) > 1 else states[0])
        return recovery.recover(self.before, status, lambda s: s["reconstructing"] is False,
                                lambda: self.calls.append("owner_check"), elapsed=elapsed,
                                monotonic=lambda: self.clock,
                                sleep=lambda t: setattr(self, "clock", self.clock + t))

    def test_gap_recovers_without_any_command(self):
        pending = dict(self.before, reconstructing=True)
        after, proof = self.run_recovery([pending, self.before])
        self.assertEqual(proof["status"], "RECOVERED")
        self.assertEqual(after, self.before)
        self.assertEqual(set(self.calls), {"status", "owner_check"})

    def test_missing_reconstruction_stops_at_30_seconds(self):
        with self.assertRaisesRegex(recovery.RecoveryStop, "within_30s"):
            self.run_recovery([dict(self.before, reconstructing=True)])
        self.assertAlmostEqual(self.clock, 30)

    def test_time_already_spent_counts_against_bound(self):
        with self.assertRaisesRegex(recovery.RecoveryStop, "within_30s"):
            self.run_recovery([dict(self.before, reconstructing=True)], elapsed=29)
        self.assertAlmostEqual(self.clock, 1)

    def test_position_mismatch_is_fatal(self):
        after = copy.deepcopy(self.before)
        after["positions"][0]["xpos"] = 1
        with self.assertRaisesRegex(recovery.RecoveryStop, "positions_differ"):
            self.run_recovery([after])

    def test_working_order_mismatch_is_fatal(self):
        row = dict(client_order_id="owned", order_id=10, isin_id=1, side="buy", price="1",
                   quantity=1, executed=0, remaining=1, state="Working", instance_owned=True,
                   operator_action_required=False)
        self.before["orders"] = [row]
        after = copy.deepcopy(self.before)
        after["orders"][0].update(state="Cancelled", remaining=0)
        with self.assertRaisesRegex(recovery.RecoveryStop, "positions_differ"):
            self.run_recovery([after])

    def test_incomplete_inventory_never_compares_as_flat(self):
        self.before["reconstructing"] = True
        with self.assertRaisesRegex(recovery.RecoveryStop, "pre_gap_complete"):
            self.run_recovery([self.before])

    def test_safety_alert_and_uncertain_command_still_stop(self):
        with self.assertRaisesRegex(recovery.RecoveryStop, "native_safety_alert"):
            self.run_recovery([dict(self.before, journal_failed=True)])
        with self.assertRaisesRegex(recovery.RecoveryStop, "uncertain_command"):
            self.run_recovery([dict(self.before, queued=1)])


if __name__ == "__main__":
    unittest.main()
