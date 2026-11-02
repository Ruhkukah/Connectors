"""Exact CLI intents, unknown-outcome refusal and exposure-reducing cleanup."""
import copy
from dataclasses import replace
import importlib
import unittest
from unittest.mock import patch

from ..commands import GuardedCli
from ..scope import HarnessStop, choose_candidate
from .fixtures import ISIN, NOW, status
from .test_scope import proof, scope


class Clock:
    def __init__(self):
        self.elapsed = 0.0

    def seconds(self):
        return self.elapsed

    def ns(self):
        return NOW + round(self.elapsed * 1_000_000_000)

    def sleep(self, seconds):
        self.elapsed += seconds


def working(client='q-partial-offline', quantity=8, executed=7, *, order_id=501):
    return dict(client_order_id=client, isin_id=ISIN, instance_owned=True,
                operator_action_required=False, state='PartFilled' if executed else 'Working',
                order_id=order_id, quantity=quantity, executed=executed,
                remaining=quantity-executed, execution_baseline_known=True)


class CommandBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.scope, self.state, self.calls, self.clock = scope(), status(), [], Clock()
        self.identity = proof(self.scope)
        self.invoke_result = dict(ok=True)

        def invoke(tokens):
            self.calls.append(tokens)
            if isinstance(self.invoke_result, Exception):
                raise self.invoke_result
            return self.invoke_result

        self.cli = GuardedCli(self.scope, invoke, lambda: copy.deepcopy(self.state),
                              lambda: self.identity, self.clock.ns, self.clock.seconds)

    def add(self):
        candidate = choose_candidate(self.scope, self.state, self.identity, self.clock.ns())
        self.cli.place('q-partial-offline', candidate)
        return candidate

    def own_partial(self, executed=7, quantity=8):
        self.state['orders'] = [working(quantity=quantity, executed=executed)]
        self.state['positions'] = [dict(isin_id=ISIN, xpos=executed)]
        self.cli.intents['q-partial-offline'] = quantity

    def move(self, quantity, price):
        self.cli.move('q-partial-offline', quantity, price, expected_order_id=501,
                      expected_executed=self.state['orders'][0]['executed'])

    def test_aggressive_exception_is_one_exact_day_buy_for_selected_isin(self):
        self.add()
        self.assertEqual(self.calls, [('plaza2', 'cmd',
                                      'place q-partial-offline 101 buy 8 10 day')])
        self.assertEqual(self.cli.intents, {'q-partial-offline': 8})

    def test_candidate_change_refuses_add_before_any_cli_call(self):
        candidate = choose_candidate(self.scope, self.state, self.identity, self.clock.ns())
        self.state['instruments'][0]['book']['best_ask']['quantity'] = 1
        with self.assertRaisesRegex(HarnessStop, 'partial_candidate_changed_before_add'):
            self.cli.place('q-partial-offline', candidate)
        self.assertEqual(self.calls, [])
        self.assertEqual(self.cli.intents, {})

    def test_oversize_candidate_and_foreign_client_are_not_scoped_exceptions(self):
        candidate = choose_candidate(self.scope, self.state, self.identity, self.clock.ns())
        for client, value in (('outside-prefix', candidate),
                              ('q-partial-offline', replace(candidate, quantity=9)),
                              ('q-partial-offline', replace(candidate, session_id=0)),
                              ('q-partial-offline', replace(candidate, trade_lifenum=99))):
            with self.subTest(client=client, candidate=value), self.assertRaises(HarnessStop):
                self.cli.place(client, value)
        self.assertEqual(self.calls, [])

    def test_unknown_add_has_one_intent_one_send_and_never_retries(self):
        self.invoke_result = TimeoutError('synthetic transport timeout')
        with self.assertRaisesRegex(HarnessStop, 'partial_command_outcome_uncertain'):
            self.add()
        self.clock.sleep(.3)
        candidate = choose_candidate(self.scope, self.state, self.identity, self.clock.ns())
        for client in ('q-partial-offline', 'q-partial-different'):
            with self.assertRaisesRegex(HarnessStop, 'partial_uncertain_add_never_retried'):
                self.cli.place(client, candidate)
        self.assertEqual(len(self.calls), 1)

    def test_negative_or_non_json_command_response_stops_exposure(self):
        for response in (dict(ok=False), dict(ok=1), None, 'accepted'):
            with self.subTest(response=response):
                self.setUp()
                self.invoke_result = response
                with self.assertRaisesRegex(HarnessStop, 'partial_command_refused_or_uncertain'):
                    self.add()
                self.assertTrue(self.cli.uncertain)

    def test_rate_guard_refuses_more_than_five_posts_per_second(self):
        self.own_partial()
        for _ in range(5):
            self.cli.cancel('q-partial-offline')
            self.clock.sleep(.201)
        # All five posts are more than 200 ms apart; a new immediate post is refused.
        self.cli.cancel('q-partial-offline')
        with self.assertRaisesRegex(HarnessStop, 'partial_rate_limit'):
            self.cli.cancel('q-partial-offline')
        self.assertEqual(len(self.calls), 6)

    def test_move_carries_original_total_eight_after_seven_filled(self):
        self.own_partial()
        self.move(8, 989_000)
        self.assertEqual(self.calls[-1][-1], 'move q-partial-offline 8 9.89')

    def test_move_rejects_remaining_quantity_substitution_and_nonpartial_orders(self):
        self.own_partial()
        with self.assertRaisesRegex(HarnessStop, 'partial_move_original_total_or_partial_unproved'):
            self.move(1, 989_000)
        for executed in (0, 8):
            self.own_partial(executed=executed)
            with self.assertRaisesRegex(HarnessStop, 'partial_move_original_total_or_partial_unproved'):
                self.move(8, 989_000)
        self.assertEqual(self.calls, [])

    def test_move_above_fresh_bid_is_refused_even_when_below_ask(self):
        self.own_partial()
        for price in (991_000, 999_000, 1_001_000, 989_001):
            with self.subTest(price=price), self.assertRaises(HarnessStop):
                self.move(8, price)
        self.assertEqual(self.calls, [])

    def test_unowned_or_other_instrument_order_never_moves_or_cancels(self):
        for changes in (dict(instance_owned=False), dict(isin_id=999),
                        dict(operator_action_required=True), dict(order_id=0)):
            self.own_partial()
            self.state['orders'][0].update(changes)
            with self.subTest(changes=changes):
                for action in (lambda: self.move(8, 989_000),
                               lambda: self.cli.cancel('q-partial-offline')):
                    with self.assertRaises(HarnessStop):
                        action()
        self.assertEqual(self.calls, [])

    def test_unknown_cancel_stops_move_and_subsequent_exposure(self):
        self.own_partial()
        self.invoke_result = TimeoutError('synthetic cancellation uncertainty')
        with self.assertRaisesRegex(HarnessStop, 'partial_command_outcome_uncertain'):
            self.cli.cancel('q-partial-offline')
        self.clock.sleep(.3)
        with self.assertRaisesRegex(HarnessStop, 'partial_uncertain_command_blocks_move'):
            self.move(8, 989_000)
        self.assertEqual(len(self.calls), 1)

    def test_flatten_eight_and_ten_only_reduces_exact_existing_position_via_ioc(self):
        for position in (8, 10, -8, -10):
            self.setUp()
            self.state['positions'] = [dict(isin_id=ISIN, xpos=position)]
            side = 'sell' if position > 0 else 'buy'
            price = 990_000 if side == 'sell' else 1_000_000
            self.cli.flatten('q-partial-flat-offline', side, abs(position), price)
            self.assertEqual(self.calls[-1][-1],
                             f'place q-partial-flat-offline 101 {side} {abs(position)} '
                             f'{"9.9" if side == "sell" else "10"} ioc')

    def test_flatten_cannot_add_exposure_overshoot_or_use_noncurrent_price(self):
        self.state['positions'] = [dict(isin_id=ISIN, xpos=8)]
        for side, quantity, price in (('buy', 8, 1_000_000), ('sell', 9, 990_000),
                                      ('sell', 8, 989_000)):
            with self.subTest(side=side, quantity=quantity, price=price), self.assertRaises(HarnessStop):
                self.cli.flatten('q-partial-flat-offline', side, quantity, price)
        self.assertEqual(self.calls, [])

    def test_cleanup_may_reduce_part_of_position_without_crossing_zero(self):
        self.state['positions'] = [dict(isin_id=ISIN, xpos=8)]
        self.cli.flatten('q-partial-flat-offline', 'sell', 7, 990_000)
        self.assertEqual(self.calls[-1][-1], 'place q-partial-flat-offline 101 sell 7 9.9 ioc')

    def test_flatten_refuses_any_still_working_order(self):
        self.own_partial()
        with self.assertRaisesRegex(HarnessStop, 'partial_flatten_with_live_order_refused'):
            self.cli.flatten('q-partial-flat-offline', 'sell', 7, 990_000)
        self.assertEqual(self.calls, [])

    def test_import_and_entry_point_cannot_start_or_arm_process(self):
        with patch('subprocess.Popen', side_effect=AssertionError('unexpected process')):
            package = importlib.import_module('scripts.test_rehearsal.partial_test')
            self.assertTrue(package.REVIEW_ONLY)
            entry = importlib.import_module('scripts.test_rehearsal.partial_test.__main__')
            with self.assertRaisesRegex(SystemExit, 'REVIEW_ONLY'):
                entry.main()


if __name__ == '__main__':
    unittest.main()
