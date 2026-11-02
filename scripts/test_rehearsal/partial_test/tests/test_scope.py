"""Exercise the same immutable scope at candidate, CLI and guardian boundaries."""
import copy
from dataclasses import replace
import unittest

from ..scope import (FrozenScope, HarnessStop, RuntimeProof, choose_candidate,
                     guardian_check, passive_move_price, validate_runtime)
from .fixtures import ISIN, NOW, SYMBOL, status


def scope(**changes):
    value = dict(authorized_date='2026-10-15', source_head='a' * 40,
                 binary_sha256='b' * 64, ci_run_id=123, login='offline-login',
                 ext_namespace='100000:199999', instance_id='offline-instance',
                 instrument=SYMBOL, isin_id=ISIN, quantity_limit=10,
                 position_limit=10, fixed_ask_budget_scaled=10_000_000)
    value.update(changes)
    return FrozenScope(**value)


def proof(value, **changes):
    fields = dict(scope_digest=value.digest, source_head=value.source_head,
                  binary_sha256=value.binary_sha256, login=value.login,
                  ext_namespace=value.ext_namespace, instance_id=value.instance_id,
                  owned_product=True, sole_instance=True, remaining_ids=7000,
                  observed_utc_ns=NOW, reservation_coverage=True,
                  reservation_negative_control=True, original_allocation_unchanged=True,
                  ci_run_id=value.ci_run_id, ci_head=value.source_head, ci_jobs_passed=True)
    fields.update(changes)
    return RuntimeProof(**fields)


class ScopeTests(unittest.TestCase):
    def setUp(self):
        self.scope = scope()
        self.proof = proof(self.scope)
        self.state = status()

    def assert_refused(self, state, code=None, *, identity=None):
        with self.assertRaises(HarnessStop) as caught:
            choose_candidate(self.scope, state, identity or self.proof, NOW)
        if code:
            self.assertEqual(str(caught.exception), code)

    def test_ask_size_plus_one_selects_two_eight_or_ten(self):
        for ask_size, expected in ((1, 2), (7, 8), (9, 10)):
            with self.subTest(ask_size=ask_size):
                candidate = choose_candidate(self.scope, status(ask_size=ask_size),
                                             self.proof, NOW)
                self.assertEqual(candidate.quantity, expected)
                self.assertEqual(candidate.price_scaled, 1_000_000)
                self.assertEqual(candidate.ask_size, ask_size)

    def test_ten_at_best_ask_does_not_allow_eleven_lot_order(self):
        self.assertIsNone(choose_candidate(self.scope, status(ask_size=10),
                                          self.proof, NOW))

    def test_frozen_notional_cannot_increase_with_market_price(self):
        self.assertIsNone(choose_candidate(self.scope,
                                          status(ask_size=9, ask=1_001_000),
                                          self.proof, NOW))

    def test_native_limits_have_to_match_selected_quantity_scope(self):
        self.assert_refused(status(quantity_limit=2),
                            'partial_effective_limits_outside_scope')

    def test_native_position_notional_and_open_order_limits_are_exact(self):
        for path, changed in (('max_position', 2), ('max_quote_notional_scaled', 10_000_001)):
            state = copy.deepcopy(self.state)
            state['configuration']['risk']['instruments'][0][path] = changed
            self.assert_refused(state, 'partial_effective_limits_outside_scope')
        state = copy.deepcopy(self.state)
        state['configuration']['risk']['max_open_orders'] = 5
        self.assert_refused(state, 'partial_effective_limits_outside_scope')

    def test_launcher_and_guardian_share_quantity_and_position_ten(self):
        args = self.scope.launcher_arguments()
        flags = dict(zip(args[:-2:2], args[1:-2:2]))
        self.assertEqual(flags['--max-quantity'], '10')
        self.assertEqual(flags['--max-position'], '101=10')
        self.assertEqual(flags['--max-notional'], '101=100')
        self.assertEqual(flags['--max-open-orders'], '1')
        self.assertEqual(flags['--max-commands-per-second'], '5')
        self.assertEqual(flags['--environment'], 'test')
        self.assertEqual(flags['--login-env'], 'MOEX_LOGIN')
        self.assertEqual(flags['--ext-id-range'], '100000:199999')
        self.assertEqual(args[-2:], ('--allow-orders', '--sole-instance'))
        state = copy.deepcopy(self.state)
        state['positions'] = [dict(isin_id=ISIN, xpos=8)]
        guardian_check(self.scope, state, self.proof, NOW)
        state['positions'][0]['xpos'] = 11
        with self.assertRaisesRegex(HarnessStop, 'partial_position_scope_unproved'):
            guardian_check(self.scope, state, self.proof, NOW)

    def test_scope_rejects_unbounded_quantity_attempt_time_and_rate(self):
        for patch in (dict(quantity_limit=11), dict(quantity_limit=True),
                      dict(position_limit=11), dict(max_attempts=4),
                      dict(timeout_seconds=601), dict(rate=6), dict(max_open_orders=2)):
            with self.subTest(patch=patch), self.assertRaises(HarnessStop):
                scope(**patch)

    def test_no_other_environment_identity_or_id_shortage(self):
        for patch in (dict(environment='PROD'), dict(login='other'),
                      dict(ext_namespace='other'), dict(instance_id='other'),
                      dict(owned_product=False), dict(sole_instance=False),
                      dict(binary_sha256='c' * 64), dict(remaining_ids=6999),
                      dict(reservation_coverage=False), dict(reservation_negative_control=False),
                      dict(original_allocation_unchanged=False),
                      dict(observed_utc_ns=NOW-10_000_000_001),
                      dict(observed_utc_ns=NOW+1)):
            with self.subTest(patch=patch):
                self.assert_refused(self.state, 'scope_identity_ownership_or_ids_unproved',
                                    identity=proof(self.scope, **patch))

    def test_later_date_authorization_cannot_be_reused(self):
        with self.assertRaisesRegex(HarnessStop, 'scope_authorized_date_changed'):
            validate_runtime(self.scope, self.state, self.proof, NOW + 86_400_000_000_000)

    def test_fresh_two_sided_book_and_ready_flags_are_required(self):
        for patch in ('missing_bid', 'stale', 'future', 'not_ready', 'crossed', 'off_grid'):
            state = copy.deepcopy(self.state)
            row, book = state['instruments'][0], state['instruments'][0]['book']
            if patch == 'missing_bid':
                book.pop('best_bid')
            elif patch == 'stale':
                book['stream_last_callback_utc_ns'] = NOW - 10_000_000_001
            elif patch == 'future':
                book['stream_last_callback_utc_ns'] = NOW + 1
            elif patch == 'not_ready':
                row['order_entry_ready'] = False
            elif patch == 'crossed':
                book['best_bid']['price_scaled'] = book['best_ask']['price_scaled']
            else:
                book['best_ask']['price_scaled'] += 1
            with self.subTest(patch=patch):
                self.assert_refused(state)

    def test_complete_same_epoch_reconstruction_is_mandatory(self):
        for patch in ('incomplete_listener', 'missing_listener', 'epoch', 'reconstructing'):
            state = copy.deepcopy(self.state)
            if patch == 'incomplete_listener':
                state['transport']['listeners'][0]['snapshot_complete'] = False
            elif patch == 'missing_listener':
                state['transport']['listeners'].pop()
            elif patch == 'epoch':
                state['pos_anchor']['trades_lifenum'] += 1
            else:
                state['reconstructing'] = True
            with self.subTest(patch=patch):
                self.assert_refused(state, 'partial_full_native_reconstruction_unproved')

    def test_new_attempt_requires_flatness_not_merely_ready(self):
        state = copy.deepcopy(self.state)
        state['positions'] = [dict(isin_id=ISIN, xpos=1)]
        self.assert_refused(state, 'partial_not_flat')
        state = copy.deepcopy(self.state)
        state['queued'] = 1
        self.assert_refused(state, 'partial_queued_command_unresolved')

    def test_pos_zero_is_not_flat_while_pending_fill_reservation_exists(self):
        state = copy.deepcopy(self.state)
        state['instruments'][0]['pending_fill_reservations']['buy_quantity'] = 1
        self.assert_refused(state, 'partial_pending_fill_reservation_not_flat')
        state['instruments'][0]['pending_fill_reservations']['proof_valid'] = False
        self.assert_refused(state, 'partial_pending_fill_proof_unproved')

    def test_wide_spread_move_uses_fresh_bid_rather_than_old_ask(self):
        state = status(bid=700_000, ask=1_000_000)
        self.assertEqual(passive_move_price(self.scope, state, self.proof, NOW), 699_000)
        self.assertLess(passive_move_price(self.scope, state, self.proof, NOW),
                        state['instruments'][0]['book']['best_ask']['price_scaled'] - 1_000)

    def test_lower_bound_move_falls_back_to_bid_without_crossing_limit(self):
        state = status(bid=500_000, ask=1_000_000)
        self.assertEqual(passive_move_price(self.scope, state, self.proof, NOW), 500_000)


if __name__ == '__main__':
    unittest.main()
