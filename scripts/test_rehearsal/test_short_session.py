"""Offline driver tests exercise actual queued sequence logic, never native I/O."""
import contextlib
import copy
import datetime as dt
import importlib.util
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import sequence as actual_sequence
import short_scope as scope

base = types.ModuleType('day_driver')
base.MSK = scope.MSK
base.DayStop = type('DayStop', (RuntimeError,), {})
base.TERMINAL = {'Filled', 'Cancelled', 'Rejected'}
base.sequence = actual_sequence
base.ops = types.SimpleNamespace()
base.ops.ready = lambda state, _targets, entry=True: state.get('complete') is True and (not entry or state.get('entry') is True)
base.ops.flat = lambda state: state['queued'] == 0 and all(row['xpos'] == 0 for row in state['positions']) and all(row['state'] in base.TERMINAL for row in state['orders'])
base.price_caps = types.SimpleNamespace(current_reference=lambda state, target, **_: 10000000 if state.get('books') == 'fresh' else (_ for _ in ()).throw(ValueError('current_price_book_stale')))


class Timeline:
    def __init__(self):
        self.cursor = types.SimpleNamespace(offset=0)
        self.session_states, self.instrument_states = {}, {}
        self.instrument_session = None
        self.restart_windows, self.rows = [], []
        self.kind_bytes, self.kind_counts = {}, {}
        self.path = Path(__file__)
    def update(self):
        self.cursor.offset = len(self.rows)
    def between(self, offset):
        return [row for row in self.rows if row['begin'] >= offset]
    def timing(self, _):
        return {}
    def append(self, event, data):
        index = len(self.rows)
        self.rows.append(dict(begin=index, end=index+1, event=dict(event=event, data=data)))
        self.update()


class MockDayDriver:
    def __init__(self, product, cfg, work, env, publish, *, wall=None, monotonic=None, sleep=None):
        self.product, self.cfg, self.work, self.env, self.publish = product, cfg, Path(work), env, publish
        self.wall, self.monotonic, self.sleep = wall, monotonic, sleep
        self.targets, self.timeline, self.scenarios = product.targets, Timeline(), {}
        self.failed = self.closing = self.restart_used = False
        product.status()
    def tick(self):
        self.timeline.update()
    def status(self):
        return self.product.status()
    def mark(self, name, result, begin):
        result = dict(result, journal_begin=begin, journal_end=self.timeline.cursor.offset)
        self.scenarios[name] = result
        return result
    def stage(self, name, callback):
        return self.mark(name, callback(), self.timeline.cursor.offset)
    def restart_working(self):
        raise AssertionError('native restart cannot execute in offline tests')
    def closeout(self):
        raise AssertionError('native closeout cannot execute in offline tests')


base.DayDriver = MockDayDriver
spec = importlib.util.spec_from_file_location('_short_session_under_test', Path(__file__).with_name('short_session.py'))
h = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {'day_driver': base}):
    spec.loader.exec_module(h)


def state(**changes):
    instruments = [dict(isin_id=isin, symbol=symbol, order_entry_ready=True,
                        session_terms=dict(sess_id=42, interval_valid=True, min_step_scaled=10000,
                                           lower_price_scaled=7000000, upper_price_scaled=12000000,
                                           reference_price_scaled=10000000, refdata_lifenum=5),
                        book=dict(valid=True, best_bid=dict(price_scaled=10000000, quantity=10),
                                  best_ask=dict(price_scaled=10100000, quantity=10)))
                   for isin, symbol in ((1, 'ALRS-12.26'), (2, 'CNY-12.26'))]
    return dict(dict(sess_id=42, complete=True, entry=True, books='fresh', queued=0,
                     reconstructing=False, orders=[], positions=[], instruments=instruments,
                     configuration=dict(allow_orders=True, sole_instance=True, login_configured=True,
                                        rate=5, risk=dict(max_quantity=2, max_open_orders=5,
                                        instruments=[dict(isin_id=isin, max_position=2, max_quote_notional_scaled=14000000) for isin in (1, 2)]))), **changes)


def full_recovery(**changes):
    return dict(dict(cancel_confirmed=True, flat=True, restored=True,
                     complete_reconstruction=True, client_count=1, upstreams=4), **changes)


class Clock:
    def __init__(self, date='2026-10-16'):
        self.now = scope.expected(date)['earliest_entry_utc']
    def time(self):
        return self.now
    def sleep(self, seconds):
        self.now += seconds


class Product:
    """Small native queue model: Del of an unposted Add discards it locally.

    Every actual post is timestamped, replied and committed. A rate bucket is
    shared across all waves; native risk reserves queued and working Adds.
    """
    def __init__(self, clock, timeline):
        self.clock, self.timeline = clock, timeline
        self.targets = [dict(isin_id=1, symbol='ALRS-12.26'), dict(isin_id=2, symbol='CNY-12.26')]
        self.orders, self.pending, self.posts, self.commands = {}, [], [], []
        self.user, self.max_pending, self.max_working = 0, 0, 0
        self.slow = self.fail_code = self.uncertain = False
    def poll(self):
        while self.pending:
            now = self.clock.time()
            self.posts = [stamp for stamp in self.posts if now-stamp < 1]
            if len(self.posts) >= 5:
                break
            kind, client = self.pending.pop(0)
            order = self.orders[client]
            self.user += 1
            self.posts.append(now)
            self.timeline.append('command', dict(name=kind, user_id=self.user))
            self.timeline.append('command_timing', dict(user_id=self.user, post_started_utc_ns=int(now*1e9)))
            self.timeline.append('reply', dict(user_id=self.user, msgid=179 if kind == 'AddOrder' else 177, code=14 if self.fail_code else 0))
            self.timeline.append('transaction_commit', dict(stream='FORTS_TRADE_REPL'))
            if kind == 'AddOrder':
                order.update(state='Working', order_id=100+self.user)
            else:
                order.update(state='Cancelled', remaining=0)
        self.max_pending = max(self.max_pending, len(self.pending))
    def command(self, line):
        self.commands.append(line)
        self.clock.sleep(.31 if self.slow else .003)
        if self.uncertain:
            raise RuntimeError('command_outcome_uncertain')
        fields = line.split()
        if fields[0] == 'kill':
            return dict(ok=True)
        if fields[0] == 'place':
            _, client, isin, side, quantity, price, tif = fields
            isin, quantity = int(isin), int(quantity)
            active = [order for order in self.orders.values() if order['state'] not in base.TERMINAL]
            same = [order for order in active if order['isin_id'] == isin]
            if len(active) >= 5 or sum(order['quantity'] for order in same)+quantity > 2:
                raise AssertionError('native position/open-order cap violated')
            units = actual_sequence._units(price)
            if sum(actual_sequence._units(order['price'])*order['remaining'] for order in same)+units*quantity > 14000000:
                raise AssertionError('native aggregate notional cap violated')
            self.orders[client] = dict(client_order_id=client, isin_id=isin, instance_owned=True,
                operator_action_required=False, ext_id=len(self.orders)+1, quantity=quantity,
                remaining=quantity, executed=0, side=side, price=price, state='Queued', order_id=0)
            self.pending.append(('AddOrder', client))
            self.max_working = max(self.max_working, len(active)+1)
        elif fields[0] == 'cancel':
            client = fields[1]
            # Mirrors order_manager.cpp: cancelling a queued Add removes it;
            # it produces neither an Add post nor a Del post.
            if ('AddOrder', client) in self.pending:
                self.pending.remove(('AddOrder', client))
                self.orders[client].update(state='Cancelled', remaining=0)
            else:
                self.pending.append(('DelOrder', client))
        self.poll()
        return dict(ok=True)
    def status(self):
        self.clock.sleep(.003)
        self.poll()
        return state(queued=len(self.pending), orders=copy.deepcopy(list(self.orders.values())))


class ShortTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        Path(self.directory.name, 'interaction.ndjson').touch()
        self.timeline = Timeline()
        self.product = Product(self.clock, self.timeline)
        self.cfg = dict(scope.expected('2026-10-16'), closeout_mode='flat_then_observe_clearing')
        self.env = dict(fast_dispatch=lambda *_: contextlib.nullcontext(), safe_case_failure=lambda *_: full_recovery())
        self.d = h.ShortDriver(self.product, self.cfg, self.directory.name, self.env, lambda _: None,
                              wall=self.clock.time, monotonic=self.clock.time, sleep=self.clock.sleep)
        self.d.timeline = self.timeline
        self.d.schedule['sess_id'] = 42

    def test_window_comes_from_shared_config_and_monday_fallback(self):
        self.assertEqual(self.d.schedule['cleanup_at'], scope.epoch('2026-10-16', 14, 30))
        self.cfg.update(scope.expected('2026-10-19'))
        other = h.ShortDriver(self.product, self.cfg, self.directory.name, self.env, lambda _: None,
                              wall=self.clock.time, monotonic=self.clock.time, sleep=self.clock.sleep)
        self.assertEqual(other.schedule['intervals'][0]['begin'], scope.epoch('2026-10-19', 9))

    def test_preadmission_transport_window_closes_at_real_admission(self):
        self.d.timeline.restart_windows.append(self.d.pre_admission_window)
        self.assertEqual(self.d.pre_admission_window[1], float('inf'))
        self.d.tick()
        self.assertFalse(self.d.admission_complete)
        self.d.admission()
        self.assertTrue(self.d.admission_complete)
        self.assertNotEqual(self.d.pre_admission_window[1], float('inf'))
        self.assertEqual(self.d.pre_admission_window[1], self.clock.time())

    def test_inherited_restart_status_entry_waits_transient_snapshot(self):
        snapshots = iter([state(books='stale'), state()])
        self.product.status = lambda: next(snapshots)
        self.assertEqual(self.d.status(entry=True)['books'], 'fresh')
        self.assertEqual(self.product.commands, [])

    def test_admission_one_snapshot_per_check_waits_transient_books(self):
        snapshots = iter([state(books='stale'), state(books='one-sided'), state(trading_date='2026-10-19')])
        calls = []
        self.d.status = lambda: calls.append(1) or next(snapshots)
        self.assertEqual(self.d.admission()['sess_id'], 42)
        self.assertEqual(len(calls), 3)
        self.assertEqual(self.product.commands, [])

    def test_admission_waits_known_auction_and_never_journal_grants_entry(self):
        self.d.timeline.session_states[42] = dict(fields=dict(public_state=2))
        self.assertFalse(self.d.product_entry_ready(state()))
        self.d.timeline.session_states.clear()
        bad = state(); bad['instruments'][0]['order_entry_ready'] = False
        self.assertFalse(self.d.product_entry_ready(bad))

    def test_single_instrument_preintent_wait_does_not_require_other_book(self):
        snapshot = state(); snapshot['instruments'][1]['order_entry_ready'] = False
        self.assertTrue(self.d.selected_entry_ready(snapshot, [self.product.targets[0]]))
        self.assertFalse(self.d.product_entry_ready(snapshot))

    def test_no_early_or_late_entry_and_admission_expiry_has_no_dispatch(self):
        for at in (scope.epoch('2026-10-16', 9, 59), scope.epoch('2026-10-16', 14, 30)):
            self.clock.now = at
            self.assertFalse(self.d.product_entry_ready(state()))
        self.clock.now = self.cfg['admission_latest_utc']
        with self.assertRaisesRegex(base.DayStop, 'admission_deadline'):
            self.d.admission()
        self.assertEqual(self.product.commands, [])

    def test_preintent_wait_keeps_single_dispatch_and_reports_not_sent(self):
        snapshots = iter([state(books='stale'), state()])
        self.d.status = lambda: next(snapshots)
        self.d.command('kill off')
        self.assertEqual(self.product.commands, ['kill off'])
        self.d.status = lambda: state(books='stale')
        with self.assertRaises(h.PreIntentNotSent):
            self.d.command('kill off')
        self.assertEqual(self.product.commands, ['kill off'])

    def test_sequence_preserves_preintent_refusal_and_never_retries(self):
        seq = self.d.seq('preintent')
        self.d.command = lambda _: (_ for _ in ()).throw(h.PreIntentNotSent('entry_wait_expired'))
        with self.assertRaises(h.PreIntentNotSent):
            seq.send('kill off')
        transcript = Path(self.directory.name, 'sequence-private.ndjson').read_text()
        self.assertIn('command_not_sent', transcript)
        self.assertNotIn('command_uncertain', transcript)
        self.assertEqual(self.product.commands, [])

    def test_sequence_preserves_actual_uncertain_dispatch_as_fatal(self):
        seq = self.d.seq('uncertain')
        self.d.command = lambda _: (_ for _ in ()).throw(RuntimeError('socket_timeout'))
        with self.assertRaisesRegex(actual_sequence.SequenceStop, 'command_outcome_uncertain'):
            seq.send('place once')

    def test_passive_wait_handles_exact_transients_without_dispatch(self):
        seq = self.d.seq('price')
        attempts = []
        def select(snapshot):
            attempts.append(1)
            if len(attempts) < 4:
                raise actual_sequence.SequenceStop(('current_price_book_stale', 'current_price_two_sided_book_unproved', 'passive_quote_unavailable')[len(attempts)-1])
            return 7020000
        self.assertEqual(self.d.wait_passive_price(seq, state(), select)[1], 7020000)
        self.assertEqual(len(attempts), 4)
        self.assertEqual(self.product.commands, [])

    def test_passive_wait_rejects_nontransient_or_changed_inventory(self):
        seq = self.d.seq('price')
        with self.assertRaisesRegex(actual_sequence.SequenceStop, 'off_grid'):
            self.d.wait_passive_price(seq, state(), lambda _: (_ for _ in ()).throw(actual_sequence.SequenceStop('off_grid')))
        seq.snapshot = lambda: state(positions=[dict(isin_id=1, xpos=1)])
        with self.assertRaisesRegex(base.DayStop, 'inventory_changed'):
            self.d.wait_passive_price(seq, state(), lambda _: (_ for _ in ()).throw(actual_sequence.SequenceStop('current_price_book_stale')))

    def test_first_fault_refreshes_after_slow_killoff_before_price_selection(self):
        seq = self.d.seq('fault')
        seen = []
        original = self.d.command
        def command(line):
            original(line)
            if line == 'kill off':
                self.clock.sleep(12)
        self.d.command = command
        self.d.seq = lambda _: seq
        def price(_seq, snapshot, _select, **_):
            seen.append(self.clock.time())
            raise actual_sequence.SequenceStop('sentinel_after_refresh')
        self.d.wait_passive_price = price
        with self.assertRaisesRegex(actual_sequence.SequenceStop, 'sentinel_after_refresh'):
            self.d.fault('router', 'router')
        self.assertGreater(seen[0], scope.epoch('2026-10-16', 10)+12)
        self.assertEqual(self.product.commands, ['kill off'])

    def test_burst_binds_actual_queue_without_relaxing_position_or_notional(self):
        result = self.d.burst()
        self.assertTrue(result['limiter_binding_observed'])
        self.assertEqual(result['native_posts'], 30)
        self.assertEqual(result['matching_code0_replies'], 30)
        self.assertEqual(result['max_posts_per_rolling_second'], 5)
        self.assertEqual([wave['adds'] for wave in result['waves']], [4, 4, 4, 3])
        self.assertGreater(self.product.max_pending, 0)
        self.assertEqual(self.product.max_working, 4)
        self.assertTrue(all(order['price'] == '70' for order in self.product.orders.values()))
        self.assertTrue(base.ops.flat(self.product.status()))
        self.assertEqual(len([line for line in self.product.commands if line.startswith('place ')]), 15)
        self.assertEqual(len([line for line in self.product.commands if line.startswith('cancel ')]), 15)

    def test_queued_add_cannot_be_cancelled_before_native_post(self):
        # Keep the bucket full: immediate cancel would discard this queued Add.
        self.product.posts = [self.clock.now]*5
        self.product.command('place never-posted 1 buy 1 70.2 day')
        self.assertEqual(self.product.status()['queued'], 1)
        self.product.command('cancel never-posted')
        self.assertEqual(self.product.status()['queued'], 0)
        self.assertEqual(self.timeline.rows, [])
        # The actual driver waits Working and consequently produces all30 posts.
        self.clock.sleep(1.1)
        self.assertEqual(self.d.burst()['native_posts'], 30)

    def test_slow_burst_does_not_claim_binding_from_thirty_replies_alone(self):
        self.product.slow = True
        with self.assertRaisesRegex(base.DayStop, 'binding_not_observed'):
            self.d.burst()

    def test_burst_rejects_nonzero_reply_and_missing_timing(self):
        self.product.fail_code = True
        with self.assertRaisesRegex(base.DayStop, 'reply_unproved'):
            self.d.burst()

    def test_reply_proof_rejects_duplicate_and_missing_commit(self):
        self.d.burst()
        rows = copy.deepcopy(self.timeline.rows)
        reply = next(row for row in rows if row['event']['event'] == 'reply')
        rows.append(copy.deepcopy(reply))
        with self.assertRaisesRegex(base.DayStop, 'reply_unproved'):
            h.burst_reply_proof(rows)
        rows = copy.deepcopy(self.timeline.rows); rows.pop()
        with self.assertRaisesRegex(base.DayStop, 'later_trade_commit'):
            h.burst_reply_proof(rows)

    def test_independent_failure_continues_only_after_strict_recovery(self):
        calls = []
        self.d.env['safe_case_failure'] = lambda *args: calls.append(args) or full_recovery()
        result = self.d.independent_case('case', lambda: (_ for _ in ()).throw(base.DayStop('measurement_unobserved')))
        self.assertEqual(result['status'], 'FAIL')
        self.assertTrue(result['safe_continuation']['flat'])
        self.assertEqual(len(calls), 1)
        self.assertEqual(self.d.independent_case('next', lambda: dict(status='PASS'))['status'], 'PASS')

    def test_independent_failure_stops_uncertain_mismatch_or_unproved_recovery(self):
        for code in ('command_outcome_uncertain', 'order_identity_mismatch', 'router_unrestored'):
            self.d.env['safe_case_failure'] = lambda *_: self.fail('fatal outcome cannot continue')
            with self.assertRaisesRegex(base.DayStop, code):
                self.d.independent_case('case', lambda c=code: (_ for _ in ()).throw(base.DayStop(c)))
        for change in (dict(flat=False), dict(client_count=0), dict(upstreams=3), dict(cancel_confirmed=1)):
            self.d.env['safe_case_failure'] = lambda *_, c=change: full_recovery(**c)
            with self.assertRaisesRegex(base.DayStop, 'continuation_unproved'):
                self.d.independent_case('case', lambda: (_ for _ in ()).throw(base.DayStop('not_observed')))

    def test_restart_last_explicitly_reenables_entry_after_fault_kill_switch(self):
        self.d.restart_working = lambda: dict(status='PASS', recovered_working_orders=2)
        self.assertEqual(self.d.restart_case()['recovered_working_orders'], 2)
        self.assertEqual(self.product.commands, ['kill off'])

    def test_burst_uses_one_owner_lease_and_refreshes_inside_that_lease(self):
        checks = []
        @contextlib.contextmanager
        def lease(*_):
            checks.append(1)
            self.clock.sleep(12)  # Slow preflight invalidates any prior book.
            yield
        self.d.env['fast_dispatch'] = lease
        result = self.d.burst()
        self.assertTrue(result['limiter_binding_observed'])
        self.assertEqual(checks, [1])

    def test_terminal_passive_fill_is_not_hidden_by_zero_position_lag(self):
        self.d.burst_clients = {'fresh'}
        self.product.orders['fresh'] = dict(client_order_id='fresh', state='Filled', executed=1,
            quantity=1, remaining=0, isin_id=1, side='buy', instance_owned=True)
        with self.assertRaisesRegex(base.DayStop, 'execution_mismatch'):
            self.d.burst_snapshot([])

    def test_run_order_is_burst_upstream_router_restart_then_flat_SIGTERM(self):
        calls = []
        self.d.stage = lambda name, callback: calls.append(name)
        self.d.independent_case = lambda name, callback: calls.append(name)
        self.d.run()
        self.assertEqual(calls, ['short_admission', 'rate_burst_thirty', 'one_passive_upstream_socket_loss',
                                 'one_passive_router_restart', 'planned_restart_two_working',
                                 'pre_SIGTERM_cancel_flat', 'terminal_closeout'])

    def test_pre_SIGTERM_requires_current_flatness_and_exact_cleanup(self):
        self.d.env['flatten'] = lambda *_: dict(cancel_confirmed=True, flat=True)
        self.assertEqual(self.d.prepare_empty_closeout()['status'], 'PASS')
        self.d.status = lambda: state(queued=1)
        with self.assertRaisesRegex(base.DayStop, 'complete_flatness'):
            self.d.prepare_empty_closeout()


if __name__ == '__main__':
    unittest.main(verbosity=2)
