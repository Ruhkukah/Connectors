"""End-to-end actual controller + typed CLI + committed native journal observer.

The adapter is a deterministic exchange fixture. It accepts the exact CLI token
vector, writes native-shaped transactions/replies and updates authoritative POS;
it does not replace controller, guard, cursor or observer implementation.
"""
import copy
from dataclasses import replace
from pathlib import Path
import tempfile
import unittest

from ..commands import GuardedCli
from ..evidence import GeneralCommittedDealObserver
from ..harness import PartialHarness, verify_independent_flat, verify_terminal
from ..scope import HarnessStop
from .fixtures import ISIN, LIFENUM, NOW, append_deal, append_event, status
from .test_commands import Clock, working
from .test_scope import proof, scope


class ExchangeFixture:
    def __init__(self, path, quantity=8, mode='partial'):
        self.path, self.quantity, self.mode = path, quantity, mode
        self.clock = Clock()
        self.scope = scope(timeout_seconds=1, max_attempts=1)
        self.state = status(ask_size=quantity-1, bid=700_000, ask=1_000_000)
        self.calls, self.timestamps, self.records = [], [], []
        self.day_snapshots, self.deal_id, self.probes = 0, 900, 0
        self.position = 0
        self.raced = False
        self.delayed_credit = False
        self.flat_receipt_override = None
        self.cli = GuardedCli(self.scope, self.invoke, self.snapshot, self.proof,
                              self.clock.ns, self.clock.seconds)

    def proof(self):
        return proof(self.scope, observed_utc_ns=self.clock.ns())

    def deal(self, row, quantity, side='buy', credit=True):
        self.deal_id += 1
        append_deal(self.path, client=row['client_order_id'], order_id=row['order_id'],
                    quantity=quantity, deal=self.deal_id, revision=self.deal_id,
                    side=side, credit=credit)

    def snapshot(self):
        live_days = [x for x in self.state['orders']
                     if not x['client_order_id'].startswith('q-partial-flat-')]
        if live_days:
            self.day_snapshots += 1
        if self.mode == 'full_rising' and self.day_snapshots:
            book = self.state['instruments'][0]['book']
            book['best_bid']['price_scaled'] = 1_100_000
            book['best_ask']['price_scaled'] = 1_200_000
        if self.mode == 'race' and self.day_snapshots >= 3 and not self.raced:
            row = live_days[0]
            self.deal(row, 1)
            row.update(state='Filled', executed=self.quantity, remaining=0)
            self.position += 1
            self.raced = True
        if self.mode == 'partial_race' and self.day_snapshots >= 4 and not self.raced:
            row = live_days[0]
            self.deal(row, 1)
            row.update(executed=row['executed']+1, remaining=row['remaining']-1)
            self.position += 1
            self.raced = True
        if self.mode == 'full_lag' and self.day_snapshots >= 3 and not self.delayed_credit:
            row = live_days[0]
            append_event(self.path, 'trade', dict(client_order_id=row['client_order_id'],
                order_id=row['order_id'], deal_id=self.deal_id, quantity=self.quantity))
            self.delayed_credit = True
        self.state['positions'] = [dict(isin_id=ISIN, xpos=self.position)]
        self.state['instruments'][0]['book']['stream_last_callback_utc_ns'] = self.clock.ns()
        return copy.deepcopy(self.state)

    def invoke(self, tokens):
        self.calls.append(tokens)
        self.timestamps.append(self.clock.seconds())
        assert tokens[:2] == ('plaza2', 'cmd')
        words = tokens[-1].split()
        verb, client = words[:2]
        if verb == 'place':
            isin, side, quantity, price, tif = (int(words[2]), words[3],
                                              int(words[4]), words[5], words[6])
            assert isin == ISIN
            if tif == 'day':
                if self.mode == 'unknown_add_missing':
                    raise TimeoutError('synthetic uncertain Add without known exchange identity')
                executed = (0 if self.mode in ('zero', 'none') else quantity
                            if self.mode in ('full', 'full_lag', 'full_rising') else 7
                            if self.mode == 'partial_race' else quantity-1)
                row = working(client, quantity, executed)
                row['price'] = price
                if self.mode == 'zero':
                    row.update(state='Cancelled', remaining=0)
                elif executed == quantity:
                    row.update(state='Filled')
                self.state['orders'].append(row)
                self.position += executed
                if executed:
                    self.deal(row, executed, credit=self.mode not in ('missing_credit', 'full_lag'))
                if self.mode == 'unknown_add':
                    raise TimeoutError('synthetic uncertain Add, accepted remotely')
            else:
                assert tif == 'ioc'
                assert 0 < quantity <= abs(self.position)
                assert side == ('sell' if self.position > 0 else 'buy')
                row = working(client, quantity, quantity, order_id=900 + len(self.calls))
                row.update(state='Filled', remaining=0, price=price)
                self.state['orders'].append(row)
                self.deal(row, quantity, side)
                self.position += quantity if side == 'buy' else -quantity
        elif verb == 'move':
            row = next(x for x in self.state['orders'] if x['client_order_id'] == client)
            assert int(words[2]) == self.quantity  # Regime 3 carries original total.
            old_id, new_id = row['order_id'], row['order_id'] + 100
            row.update(order_id=new_id, price=words[3])
            append_event(self.path, 'command', dict(name='MoveOrder',
                client_order_id=client, user_id=77,
                fields=dict(order_id1=old_id, regime=3)))
            append_event(self.path, 'reply', dict(user_id=77, msgid=176,
                                                 code=0, order_id1=new_id))
            append_event(self.path, 'transaction_commit', dict(stream='FORTS_TRADE_REPL'))
            if self.mode == 'replacement_fill':
                self.deal(row, 1)
                row.update(state='Filled', executed=self.quantity, remaining=0)
                self.position += 1
        elif verb == 'cancel':
            if self.mode == 'unknown_cancel':
                raise TimeoutError('synthetic uncertain cancellation')
            row = next(x for x in self.state['orders'] if x['client_order_id'] == client)
            row.update(state='Cancelled', remaining=0)
            append_event(self.path, 'command', dict(name='DelOrder',
                client_order_id=client, user_id=78, fields=dict(order_id=row['order_id'])))
            append_event(self.path, 'reply', dict(user_id=78, msgid=177, code=0))
            append_event(self.path, 'transaction_commit', dict(stream='FORTS_TRADE_REPL'))
        else:
            raise AssertionError('unexpected CLI verb')
        return dict(ok=True)

    def independent_flat(self):
        self.clock.sleep(.001)  # Probe must be collected after the last native command.
        self.probes += 1
        if self.flat_receipt_override and self.probes > 1:
            return self.flat_receipt_override
        return dict(scope_digest=self.scope.digest, complete=True, same_instance=True,
                    observed_utc_ns=self.clock.ns(), native_state=self.snapshot(),
                    runtime_proof=self.proof(),
                    working_orders=sum(x['state'] not in ('Filled', 'Cancelled', 'Rejected')
                                       for x in self.state['orders']),
                    queued=0, nonzero_positions=int(self.position != 0))

    def run(self):
        harness = PartialHarness(
            self.scope, self.cli,
            lambda offset, client, orders, isin, quantity, life, **kwargs:
                GeneralCommittedDealObserver(self.path, offset, client, orders,
                                             isin, quantity, life, **kwargs),
            lambda: self.path.stat().st_size, self.independent_flat,
            lambda event, data: self.records.append((event, data)),
            self.clock.seconds, self.clock.sleep)
        self.controller = harness
        return harness.run()


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'interaction.ndjson'
        self.path.touch()

    def fixture(self, quantity=8, mode='partial'):
        self.path.write_text('')
        return ExchangeFixture(self.path, quantity, mode)

    def test_actual_q2_q8_q10_committed_partial_move_cancel_flat_lifecycle(self):
        for quantity in (2, 8, 10):
            with self.subTest(quantity=quantity):
                exchange = self.fixture(quantity)
                result = exchange.run()
                self.assertEqual(result['status'], 'PASS')
                self.assertEqual(result['quantity'], quantity)
                self.assertEqual(result['executed_before_move'], quantity-1)
                self.assertEqual(result['remaining_before_move'], 1)
                commands = [call[-1] for call in exchange.calls]
                self.assertEqual(commands[0], f'place q-partial-1 101 buy {quantity} 10 day')
                self.assertEqual(commands[1], f'move q-partial-1 {quantity} 6.99')
                self.assertEqual(commands[2], 'cancel q-partial-1')
                self.assertEqual(commands[3],
                                 f'place q-partial-flat-1-0 101 sell {quantity-1} 7 ioc')
                self.assertEqual(exchange.position, 0)
                self.assertEqual(exchange.probes, 2)
                self.assertTrue(all(b-a >= .2 for a, b in
                                    zip(exchange.timestamps, exchange.timestamps[1:])))

    def test_full_fill_is_accounted_and_flattened_but_never_moved(self):
        exchange = self.fixture(mode='full')
        result = exchange.run()
        self.assertEqual(result['status'], 'NOT_OBSERVED')
        self.assertEqual(result['reason'], 'all_lots_filled_before_move')
        self.assertFalse(any(call[-1].startswith('move ') for call in exchange.calls))
        self.assertEqual(exchange.position, 0)

    def test_rising_price_cleanup_chunks_full_ten_lot_position_under_frozen_notional(self):
        exchange = self.fixture(quantity=10, mode='full_rising')
        result = exchange.run()
        self.assertEqual(result['status'], 'NOT_OBSERVED')
        self.assertEqual(result['reason'], 'all_lots_filled_before_move')
        flatten = [call[-1] for call in exchange.calls if call[-1].endswith(' ioc')]
        self.assertEqual(flatten, ['place q-partial-flat-1-0 101 sell 9 11 ioc',
                                  'place q-partial-flat-1-1 101 sell 1 11 ioc'])
        self.assertEqual(exchange.position, 0)
        self.assertEqual(exchange.scope.fixed_ask_budget_scaled, 10_000_000)

    def test_async_full_fill_credit_lag_waits_for_native_proof_without_false_failure(self):
        exchange = self.fixture(mode='full_lag')
        result = exchange.run()
        self.assertTrue(exchange.delayed_credit)
        self.assertEqual(result['status'], 'NOT_OBSERVED')
        self.assertEqual(result['reason'], 'all_lots_filled_before_move')
        self.assertEqual(exchange.position, 0)

    def test_replacement_fill_after_move_commit_tracks_new_alias_and_flattens(self):
        exchange = self.fixture(mode='replacement_fill')
        result = exchange.run()
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['executed_before_move'], 7)
        self.assertNotEqual(result['original_order_id'], result['replacement_order_id'])
        self.assertFalse(any(call[-1].startswith('cancel ') for call in exchange.calls))
        self.assertTrue(exchange.calls[-1][-1].endswith('sell 8 7 ioc'))
        self.assertEqual(exchange.position, 0)

    def test_zero_execution_cancel_is_not_a_partial_and_posts_no_flatten(self):
        exchange = self.fixture(mode='zero')
        result = exchange.run()
        self.assertEqual(result['status'], 'NOT_OBSERVED')
        self.assertEqual(len(exchange.calls), 1)
        self.assertEqual(exchange.position, 0)

    def test_three_attempt_budget_is_hard_and_each_add_has_unique_client(self):
        exchange = self.fixture(mode='zero')
        exchange.scope = replace(exchange.scope, max_attempts=3)
        exchange.cli.scope = exchange.scope
        result = exchange.run()
        self.assertEqual(result['status'], 'NOT_OBSERVED')
        self.assertEqual(result['attempts'], 3)
        commands = [call[-1] for call in exchange.calls]
        self.assertEqual([line.split()[1] for line in commands],
                         ['q-partial-1', 'q-partial-2', 'q-partial-3'])
        self.assertTrue(all(line.endswith(' day') for line in commands))

    def test_missing_partial_credit_is_a_correctness_stop_not_new_attempt(self):
        exchange = self.fixture(mode='missing_credit')
        exchange.scope = replace(exchange.scope, max_attempts=3)
        exchange.cli.scope = exchange.scope
        with self.assertRaises(HarnessStop):
            exchange.run()
        self.assertEqual(sum(call[-1].endswith(' day') for call in exchange.calls), 1)
        self.assertFalse(any(call[-1].startswith('move ') for call in exchange.calls))
        self.assertFalse(any(event == 'partial_result' for event, _ in exchange.records))

    def test_no_fill_observation_is_bounded_and_cancels_remainder(self):
        exchange = self.fixture(mode='none')
        result = exchange.run()
        self.assertEqual(result['status'], 'NOT_OBSERVED')
        self.assertTrue(exchange.calls[-1][-1].startswith('cancel '))
        self.assertLess(exchange.clock.seconds(), 2)
        self.assertEqual(exchange.position, 0)

    def test_new_fill_between_trigger_and_move_never_posts_move(self):
        exchange = self.fixture(mode='race')
        result = exchange.run()
        self.assertTrue(exchange.raced)
        self.assertEqual(result['status'], 'NOT_OBSERVED')
        self.assertEqual(result['reason'], 'second_fill_raced_before_move')
        self.assertFalse(any(call[-1].startswith('move ') for call in exchange.calls))
        self.assertEqual(exchange.position, 0)

    def test_final_cli_snapshot_new_partial_cannot_use_old_committed_witness(self):
        exchange = self.fixture(quantity=10, mode='partial_race')
        result = exchange.run()
        self.assertTrue(exchange.raced)
        self.assertEqual(result['status'], 'NOT_OBSERVED')
        self.assertEqual(result['reason'], 'second_fill_raced_before_move')
        self.assertFalse(any(call[-1].startswith('move ') for call in exchange.calls))
        self.assertEqual(exchange.position, 0)

    def test_controller_is_one_shot_even_after_successful_cleanup(self):
        exchange = self.fixture()
        self.assertEqual(exchange.run()['status'], 'PASS')
        calls = list(exchange.calls)
        with self.assertRaisesRegex(HarnessStop, 'partial_controller_is_one_shot'):
            exchange.controller.run()
        self.assertEqual(exchange.calls, calls)

    def test_unknown_add_is_not_retried_and_known_order_is_cleaned_up(self):
        exchange = self.fixture(mode='unknown_add')
        with self.assertRaisesRegex(HarnessStop, 'partial_command_outcome_uncertain'):
            exchange.run()
        day_adds = [x for x in exchange.calls if x[-1].endswith(' day')]
        self.assertEqual(len(day_adds), 1)
        self.assertTrue(any(x[-1].startswith('cancel ') for x in exchange.calls))
        self.assertEqual(exchange.position, 0)

    def test_unknown_add_without_order_identity_cannot_claim_flat_cleanup(self):
        exchange = self.fixture(mode='unknown_add_missing')
        with self.assertRaisesRegex(HarnessStop, 'partial_uncertain_add_terminal_identity_unproved'):
            exchange.run()
        self.assertEqual(len(exchange.calls), 1)
        self.assertEqual(exchange.probes, 1)
        self.assertEqual(exchange.records[-1][0], 'partial_cleanup_failed')
        self.assertEqual(exchange.records[-1][1]['initiating_code'],
                         'partial_command_outcome_uncertain')

    def test_uncertain_cancellation_never_posts_flatten_or_next_attempt(self):
        exchange = self.fixture(mode='unknown_cancel')
        with self.assertRaisesRegex(HarnessStop, 'partial_command_outcome_uncertain'):
            exchange.run()
        self.assertEqual(sum(x[-1].endswith(' day') for x in exchange.calls), 1)
        self.assertFalse(any(x[-1].endswith(' ioc') for x in exchange.calls))
        self.assertEqual(exchange.records[-1][0], 'partial_cleanup_failed')
        self.assertNotEqual(exchange.position, 0)

    def test_incomplete_independent_snapshot_cannot_turn_scenario_into_pass(self):
        exchange = self.fixture()
        exchange.flat_receipt_override = dict(scope_digest=exchange.scope.digest,
                                              complete=False, same_instance=True,
                                              working_orders=0, queued=0, nonzero_positions=0)
        with self.assertRaisesRegex(HarnessStop, 'partial_independent_flatness_unproved'):
            exchange.run()
        self.assertFalse(any(event == 'partial_result' for event, _ in exchange.records))

    def test_recent_but_pre_add_flat_snapshot_cannot_prove_post_cleanup_flatness(self):
        exchange = self.fixture()
        exchange.flat_receipt_override = dict(scope_digest=exchange.scope.digest,
            complete=True, same_instance=True, working_orders=0, queued=0,
            nonzero_positions=0, observed_utc_ns=NOW, native_state=status(),
            runtime_proof=proof(exchange.scope))
        with self.assertRaisesRegex(HarnessStop, 'partial_independent_flatness_stale'):
            exchange.run()
        self.assertEqual(exchange.position, 0)
        self.assertFalse(any(event == 'partial_result' for event, _ in exchange.records))

    def test_independent_flatness_requires_current_matching_scope_and_exact_counts(self):
        selected = scope()
        good = dict(scope_digest=selected.digest, complete=True, same_instance=True,
                    working_orders=0, queued=0, nonzero_positions=0,
                    observed_utc_ns=NOW, native_state=status(), runtime_proof=proof(selected))
        verify_independent_flat(selected, good, NOW)
        for patch in (dict(scope_digest='wrong'), dict(complete=1), dict(same_instance=False),
                      dict(working_orders=1), dict(working_orders=False), dict(queued=1),
                      dict(nonzero_positions=1)):
            with self.subTest(patch=patch), self.assertRaises(HarnessStop):
                verify_independent_flat(selected, dict(good, **patch), NOW)

    def test_terminal_requires_cancel_flat_zero_exit_zero_clients_and_guardian(self):
        selected = scope()
        good = dict(scope_digest=selected.digest, terminal=True,
                    closeout=dict(cancel_confirmed=True, flat=True, product_exit=0,
                                  after=dict(client_count=0), shutdown_error=False,
                                  inventory_error=False), guardian=dict(no_failure=True))
        self.assertTrue(verify_terminal(selected, good))
        cases = []
        for key, value in (('cancel_confirmed', False), ('flat', False),
                           ('product_exit', 8), ('product_exit', False),
                           ('shutdown_error', True), ('inventory_error', True)):
            bad = copy.deepcopy(good)
            bad['closeout'][key] = value
            cases.append(bad)
        bad = copy.deepcopy(good)
        bad['closeout']['after']['client_count'] = 1
        cases.append(bad)
        bad = copy.deepcopy(good)
        bad['guardian']['no_failure'] = False
        cases.append(bad)
        bad = copy.deepcopy(good)
        bad['terminal'] = 1
        cases.append(bad)
        for bad in cases:
            with self.subTest(receipt=bad), self.assertRaises(HarnessStop):
                verify_terminal(selected, bad)


if __name__ == '__main__':
    unittest.main()
