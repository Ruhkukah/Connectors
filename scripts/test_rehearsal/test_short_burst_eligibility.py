"""Offline burst regressions use real quote validation and native queue semantics.

The changing books below have the same fields as saved product status. They
exercise the driver through its real sequence/price functions; no TEST process,
socket, fault helper or controller is created.
"""
import contextlib
import unittest
from unittest.mock import patch

import price_caps as actual_price_caps
import test_short_session as fixture


class BurstEligibilityTests(unittest.TestCase):
    def setUp(self):
        fixture.ShortTests.setUp(self)
        self.addCleanup(patch.stopall)
        patch.object(fixture.h.base, 'price_caps', actual_price_caps).start()
        patch.object(actual_price_caps.time, 'time_ns', lambda: int(self.clock.time() * 1e9)).start()
        self.status_mutator = lambda state: None
        original_status = self.product.status

        def status():
            snapshot = original_status()
            for row in snapshot['instruments']:
                row['book']['stream_last_callback_utc_ns'] = int(self.clock.time() * 1e9)
            self.status_mutator(snapshot)
            return snapshot

        self.product.status = status
        self.leases = []
        self.active_lease = None
        self.lease_hook = lambda _number, _targets: None

        @contextlib.contextmanager
        def lease(product, targets):
            self.assertIs(product, self.product)
            isins = [target['isin_id'] for target in targets]
            self.assertEqual(len(isins), len(set(isins)), 'lease must bind unique selected ISINs')
            event = dict(targets=isins, begin=self.clock.time(), commands_begin=len(self.product.commands))
            self.leases.append(event)
            self.active_lease = event
            self.lease_hook(len(self.leases), targets)
            try:
                yield
            finally:
                event.update(end=self.clock.time(), commands_end=len(self.product.commands))
                self.active_lease = None

        self.d.env['fast_dispatch'] = lease

    def native_commands(self):
        return [row for row in self.timeline.rows if row['event']['event'] == 'command']

    def use_actual_supervisor_leases(self, before_preflight=lambda _number, _targets: None):
        """Compose the actual deployment lease and its bounded status wait."""
        import test_short_control as control
        runner = control.supervisor.Supervisor()
        runner.cfg, runner.closing = control.configuration(), False
        runner.work, runner.product, runner.targets = self.d.work, self.product, self.product.targets
        for target in runner.targets:
            target['quote_cap_scaled'] = 14_000_000
        runner.verify_owner = lambda product: self.assertIs(product, self.product)
        runner._cheap_owner = lambda product: self.assertIs(product, self.product)
        runner.rt = control.types.SimpleNamespace(day_check=lambda *_args, **_kwargs: None,
            configuration_gate=lambda *_args: None)
        runner.ops = fixture.base.ops
        patch.object(control.supervisor.time, 'time', self.clock.time).start()
        patch.object(control.supervisor.time, 'monotonic', self.clock.time).start()
        patch.object(control.supervisor.time, 'sleep', self.clock.sleep).start()

        @contextlib.contextmanager
        def lease(product, targets):
            event = dict(targets=[target['isin_id'] for target in targets], begin=self.clock.time(),
                commands_begin=len(self.product.commands), preflight_yielded=False)
            self.leases.append(event)
            before_preflight(len(self.leases), targets)
            try:
                with runner.fast_dispatch(product, targets):
                    event['preflight_yielded'] = True
                    self.active_lease = event
                    yield
            finally:
                self.active_lease = None
                event.update(end=self.clock.time(), commands_end=len(self.product.commands))
        self.d.env['fast_dispatch'] = lease
        return runner, control

    def complete_burst(self):
        result = self.d.burst()
        commands = self.native_commands()
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['native_posts'], 30)
        self.assertEqual(result['matching_code0_replies'], 30)
        self.assertEqual(sum(row['event']['data']['name'] == 'AddOrder' for row in commands), 15)
        self.assertEqual(sum(row['event']['data']['name'] == 'DelOrder' for row in commands), 15)
        self.assertEqual(len([row for row in self.timeline.rows if row['event']['event'] == 'transaction_commit']), 30)
        self.assertEqual(self.product.commands.count('kill off'), 1)
        self.assertTrue(result['native_rate_throttle_observed'])
        self.assertEqual(result['max_posts_per_rolling_second'], 5)
        self.assertTrue(fixture.base.ops.flat(self.product.status()))
        self.assertLessEqual(self.product.max_working, 4)
        self.assertTrue(all(order['quantity'] == 1 and order['price'] == '70' for order in self.product.orders.values()))
        return result

    @staticmethod
    def hide_book(snapshot, isin, side='best_ask'):
        next(row for row in snapshot['instruments'] if row['isin_id'] == isin)['book'][side] = None

    def test_two_eligible_instruments_complete_exact_thirty_native_posts(self):
        result = self.complete_burst()
        self.assertEqual([wave['adds'] for wave in result['waves']], [4, 4, 4, 3])
        self.assertTrue(all(set(lease['targets']) == {1, 2} for lease in self.leases))

    def test_observed_cny_ask_loss_after_twenty_four_posts_uses_alrs_two_plus_one(self):
        def mutate(snapshot):
            if len(self.native_commands()) >= 24:
                self.hide_book(snapshot, 2)
        self.status_mutator = mutate
        result = self.complete_burst()
        self.assertEqual([wave['adds'] for wave in result['waves']], [4, 4, 4, 2, 1])
        self.assertEqual([lease['targets'] for lease in self.leases[-2:]], [[1], [1]])
        self.assertEqual([order['isin_id'] for order in list(self.product.orders.values())[-3:]], [1, 1, 1])

    def test_cny_one_sided_from_start_never_blocks_eligible_alrs(self):
        self.status_mutator = lambda snapshot: self.hide_book(snapshot, 2)
        result = self.complete_burst()
        self.assertEqual([wave['adds'] for wave in result['waves']], [2] * 7 + [1])
        self.assertEqual({order['isin_id'] for order in self.product.orders.values()}, {1})
        self.assertEqual(self.product.max_working, 2)
        self.assertTrue(all(lease['targets'] == [1] for lease in self.leases))

    def test_alrs_one_sided_from_start_uses_only_eligible_cny(self):
        self.status_mutator = lambda snapshot: self.hide_book(snapshot, 1, 'best_bid')
        self.complete_burst()
        self.assertEqual({order['isin_id'] for order in self.product.orders.values()}, {2})
        self.assertEqual(self.product.max_working, 2)
        self.assertTrue(all(lease['targets'] == [2] for lease in self.leases))

    def test_neither_eligible_wait_is_bounded_and_sends_nothing(self):
        def mutate(snapshot):
            self.hide_book(snapshot, 1)
            self.hide_book(snapshot, 2)
        self.status_mutator = mutate
        started = self.clock.time()
        with self.assertRaises(fixture.h.PreIntentNotSent):
            self.d.burst()
        self.assertEqual(self.product.commands, [])
        self.assertEqual(self.native_commands(), [])
        self.assertEqual(self.leases, [])
        self.assertGreater(self.clock.time() - started, 0)
        self.assertLessEqual(self.clock.time() - started, 120.5)

    def test_drop_between_plan_and_lease_replans_before_any_add(self):
        drop = False
        def mutate(snapshot):
            if drop:
                self.hide_book(snapshot, 2)
        def hook(_number, _targets):
            nonlocal drop
            drop = True
        self.status_mutator, self.lease_hook = mutate, hook
        result = self.complete_burst()
        self.assertEqual(result['waves'][0]['adds'], 2)
        self.assertEqual({order['isin_id'] for order in self.product.orders.values()}, {1})

    def test_replan_cannot_grant_an_instrument_outside_selected_lease(self):
        switched = False
        def mutate(snapshot):
            self.hide_book(snapshot, 1 if switched else 2)
        def hook(number, _targets):
            nonlocal switched
            if number == 1:
                switched = True
        self.status_mutator, self.lease_hook = mutate, hook
        self.complete_burst()
        self.assertEqual(self.leases[0]['targets'], [1])
        self.assertEqual(self.leases[0]['commands_begin'], self.leases[0]['commands_end'])
        self.assertTrue(all(lease['targets'] == [2] for lease in self.leases[1:]))
        self.assertEqual({order['isin_id'] for order in self.product.orders.values()}, {2})

    def test_cumulative_price_wait_over_thirty_seconds_has_fresh_bounded_wave_leases(self):
        began = self.clock.time()
        def mutate(snapshot):
            elapsed = self.clock.time() - began
            self.hide_book(snapshot, 2)
            if elapsed < 18 or (len(self.native_commands()) >= 4 and elapsed < 36):
                self.hide_book(snapshot, 1)
        self.status_mutator = mutate
        self.complete_burst()
        self.assertGreater(self.clock.time() - began, 36)
        self.assertGreater(self.leases[1]['begin'] - began, 30)
        self.assertTrue(all(lease['end'] - lease['begin'] < 30 for lease in self.leases))
        self.assertTrue(all(lease['commands_end'] > lease['commands_begin'] for lease in self.leases))

    def test_slow_owner_check_refreshes_book_inside_each_new_lease(self):
        refreshed_in_lease = []
        original = self.product.status
        def status():
            snapshot = original()
            if self.active_lease is not None:
                refreshed_in_lease.append(self.active_lease)
            return snapshot
        self.product.status = status
        self.lease_hook = lambda _number, _targets: self.clock.sleep(12)
        self.complete_burst()
        self.assertTrue(all(lease in refreshed_in_lease for lease in self.leases))

    def test_selected_not_ready_is_skipped_even_with_fresh_two_sided_book(self):
        def mutate(snapshot):
            snapshot['instruments'][1]['order_entry_ready'] = False
        self.status_mutator = mutate
        self.complete_burst()
        self.assertEqual({order['isin_id'] for order in self.product.orders.values()}, {1})

    def test_stale_unselected_book_is_skipped_using_actual_callback_age(self):
        def mutate(snapshot):
            snapshot['instruments'][1]['book']['stream_last_callback_utc_ns'] -= 11_000_000_000
        self.status_mutator = mutate
        self.complete_burst()
        self.assertEqual({order['isin_id'] for order in self.product.orders.values()}, {1})

    def test_real_price_identity_grid_and_native_terms_errors_fail_closed(self):
        cases = {
            'identity': lambda row: row.update(symbol='OTHER'),
            'off_grid': lambda row: row['book']['best_bid'].update(price_scaled=10_000_001),
            'outside_bounds': lambda row: row['book']['best_ask'].update(price_scaled=13_000_000),
            'refdata_lifenum': lambda row: row['session_terms'].update(refdata_lifenum=0),
            'invalid_grid': lambda row: row['session_terms'].update(min_step_scaled=0),
        }
        for name, corrupt in cases.items():
            with self.subTest(name=name):
                snapshot = self.product.status()
                corrupt(snapshot['instruments'][0])
                with self.assertRaises((fixture.base.DayStop, fixture.actual_sequence.SequenceStop, ValueError)):
                    self.d.burst_wave_plan(self.d.seq('closed-' + name), snapshot, 15)
                self.assertEqual(self.product.commands, [])

    def test_instrument_risk_configuration_cannot_expand_for_selection(self):
        cases = {
            'position': lambda state: state['configuration']['risk']['instruments'][0].update(max_position=3),
            'notional': lambda state: state['configuration']['risk']['instruments'][0].update(max_quote_notional_scaled=20_000_001),
        }
        for name, corrupt in cases.items():
            with self.subTest(name=name):
                snapshot = self.product.status()
                corrupt(snapshot)
                with self.assertRaises((fixture.base.DayStop, fixture.actual_sequence.SequenceStop, ValueError)):
                    self.d.burst_wave_plan(self.d.seq('limits-' + name), snapshot, 15)
                self.assertEqual(self.product.commands, [])

    def test_two_q1_orders_share_frozen_cap_and_do_not_double_it(self):
        snapshot = self.product.status()
        snapshot['configuration']['risk']['instruments'][0]['max_quote_notional_scaled'] = 7_000_000
        targets, prices, _blocked = self.d.burst_wave_plan(self.d.seq('cap-one'), snapshot, 15)
        self.assertEqual(sum(target['isin_id'] == 1 for target in targets), 1)
        self.assertEqual(sum(target['isin_id'] == 2 for target in targets), 2)
        self.assertEqual(prices, {1: 7_000_000, 2: 7_000_000})
        self.assertEqual(snapshot['configuration']['risk']['instruments'][0]['max_quote_notional_scaled'], 7_000_000)

    def test_partial_capacity_never_exceeds_remaining_add_del_pairs(self):
        snapshot = self.product.status()
        for remaining in (1, 2, 3):
            with self.subTest(remaining=remaining):
                targets, _prices, _blocked = self.d.burst_wave_plan(self.d.seq('remaining'), snapshot, remaining)
                self.assertEqual(len(targets), remaining)
                self.assertLessEqual(max(sum(target['isin_id'] == isin for target in targets) for isin in (1, 2)), 2)

    def test_uncertain_add_is_attempted_once_and_never_retried_or_replanned(self):
        original = self.product.command
        def command(line):
            if line.startswith('place '):
                self.product.uncertain = True
            return original(line)
        self.product.command = command
        with self.assertRaisesRegex(fixture.base.DayStop, 'uncertain'):
            self.d.burst()
        self.assertEqual(len([line for line in self.product.commands if line.startswith('place ')]), 1)
        self.assertEqual(self.native_commands(), [])
        self.assertEqual(len(self.leases), 1)

    def test_actual_supervisor_preflight_book_race_replans_after_completed_readonly_refusal(self):
        drop_cny = False
        def mutate(snapshot):
            if drop_cny:
                self.hide_book(snapshot, 2)
        def preflight(number, _targets):
            nonlocal drop_cny
            if number == 1:
                drop_cny = True
        self.status_mutator = mutate
        self.use_actual_supervisor_leases(preflight)
        started = self.clock.time()
        self.complete_burst()
        self.assertEqual(self.leases[0]['targets'], [1, 2])
        self.assertFalse(self.leases[0]['preflight_yielded'])
        self.assertGreaterEqual(self.leases[0]['end'] - self.leases[0]['begin'], 30)
        self.assertEqual(self.leases[0]['commands_begin'], self.leases[0]['commands_end'])
        self.assertTrue(all(lease['targets'] == [1] and lease['preflight_yielded'] for lease in self.leases[1:]))
        self.assertEqual({order['isin_id'] for order in self.product.orders.values()}, {1})
        self.assertLess(self.clock.time() - started, 120)

    def test_actual_supervisor_owner_failure_is_never_eligibility_retry(self):
        runner, control = self.use_actual_supervisor_leases()
        runner.verify_owner = lambda _product: (_ for _ in ()).throw(control.Refused('short_owned_product_identity_changed'))
        with self.assertRaisesRegex(control.Refused, 'identity_changed'):
            self.d.burst()
        self.assertEqual(self.product.commands, [])
        self.assertEqual(len(self.leases), 1)

    def test_actual_supervisor_preflight_at_hard_deadline_is_not_retried(self):
        def preflight(_number, _targets):
            self.clock.now = self.d.window['hard_entry_deadline_utc']
        self.use_actual_supervisor_leases(preflight)
        with self.assertRaisesRegex(fixture.h.PreIntentNotSent, 'short_burst_dispatch_deadline'):
            self.d.burst()
        self.assertEqual(self.product.commands, [])
        self.assertEqual(len(self.leases), 1)

    def test_same_typed_refusal_after_add_is_never_retried(self):
        _runner, control = self.use_actual_supervisor_leases()
        original = self.product.command
        def command(line):
            result = original(line)
            if line.startswith('place '):
                raise control.supervisor.PreIntentNotSent('short_preintent_readiness_deadline_not_sent')
            return result
        self.product.command = command
        with self.assertRaises(control.supervisor.PreIntentNotSent):
            self.d.burst()
        self.assertEqual(len([line for line in self.product.commands if line.startswith('place ')]), 1)
        self.assertEqual(len(self.native_commands()), 1)
        self.assertEqual(len(self.leases), 1)


if __name__ == '__main__':
    unittest.main()
