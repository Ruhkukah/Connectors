"""Bounded scenario controller, transport-free until explicitly injected.

This module is not connected to a running job. Its fake-adapter regressions
exercise the same state machine that a future reviewed protected adapter uses.
"""
import time
from .scope import (HarnessStop, TERMINAL, WORKING, choose_candidate,
                    full_native_ready, passive_move_price, positive, require, validate_runtime)


def verify_independent_flat(scope, receipt, now_ns, not_before_ns=0, expected_lifenum=None):
    require(isinstance(receipt, dict) and receipt.get('scope_digest') == scope.digest and
            receipt.get('complete') is True and receipt.get('same_instance') is True and
            type(receipt.get('working_orders')) is int and receipt['working_orders'] == 0 and
            type(receipt.get('queued')) is int and receipt['queued'] == 0 and
            type(receipt.get('nonzero_positions')) is int and receipt['nonzero_positions'] == 0,
            'partial_independent_flatness_unproved')
    require(type(receipt.get('observed_utc_ns')) is int and
            not_before_ns < receipt['observed_utc_ns'] <= now_ns and
            now_ns-receipt['observed_utc_ns'] <= 10_000_000_000,
            'partial_independent_flatness_stale')
    # A complete=true summary alone is insufficient: independently collected
    # native reconstruction, identity, epoch, printed limits and zero inventory
    # must satisfy the exact same immutable scope as the command path.
    validate_runtime(scope, receipt.get('native_state', {}), receipt.get('runtime_proof'),
                     now_ns, require_flat=True, entry=False)
    if expected_lifenum is not None:
        require(receipt['native_state']['trade_replay']['accepted_lifenum'] == expected_lifenum,
                'partial_cleanup_source_epoch_changed')


def verify_terminal(scope, receipt):
    closeout = receipt.get('closeout', {})
    require(receipt.get('scope_digest') == scope.digest and receipt.get('terminal') is True and
            closeout.get('cancel_confirmed') is True and closeout.get('flat') is True and
            type(closeout.get('product_exit')) is int and closeout['product_exit'] == 0 and
            type(closeout.get('after', {}).get('client_count')) is int and
            closeout['after']['client_count'] == 0 and
            closeout.get('shutdown_error') is False and closeout.get('inventory_error') is False and
            receipt.get('guardian', {}).get('no_failure') is True,
            'partial_terminal_closeout_unproved')
    return True


class PartialHarness:
    def __init__(self, scope, cli, observer_factory, journal_offset, independent_flat,
                 record=lambda event, data: None, monotonic=time.monotonic, sleep=time.sleep):
        require(cli.scope == scope, 'partial_controller_scope_changed')
        self.scope, self.cli, self.observer_factory = scope, cli, observer_factory
        self.journal_offset, self.independent_flat, self.record = journal_offset, independent_flat, record
        self.monotonic, self.sleep = monotonic, sleep
        self.deadline = None
        self.started = False

    def _pace(self):
        if self.cli.last_command is not None:
            delay = .200001 - (self.monotonic() - self.cli.last_command)
            if delay > 0:
                self.sleep(delay)

    def _wait(self, predicate, timeout=15, entry=False):
        end = min(self.deadline + 30, self.monotonic() + timeout)
        while True:
            state = self.cli.snapshot(entry=entry)
            value = predicate(state)
            if value:
                return state, value
            require(self.monotonic() < end, 'partial_command_commit_or_snapshot_timeout')
            self.sleep(.02)

    @staticmethod
    def _order(state, client):
        rows = [x for x in state.get('orders', []) if x.get('client_order_id') == client]
        require(len(rows) <= 1, 'partial_client_identity_ambiguous')
        return rows[0] if rows else None

    def _partial(self, state, client, observer, quantity):
        require(full_native_ready(state) and
                state['trade_replay']['accepted_lifenum'] == observer.lifenum,
                'partial_observer_current_epoch_mismatch')
        row = self._order(state, client)
        if not row or row.get('state') not in WORKING or not 0 < row.get('executed', 0) < quantity:
            return None
        require(row.get('instance_owned') is True and row.get('operator_action_required') is False and
                row.get('quantity') == quantity and row.get('remaining') == quantity-row['executed'] and
                row.get('execution_baseline_known') is True and positive(row.get('order_id')),
                'partial_status_accounting_conflict')
        return row if observer.partial(row['executed']) else None

    def _cleanup(self, client, attempt, observer, offset, quantity, lifenum):
        state = self.cli.snapshot(entry=False)
        order = self._order(state, client)
        if order and order.get('state') not in TERMINAL:
            self._pace()
            self.cli.cancel(client)
            state, _ = self._wait(lambda s: (self._order(s, client) or {}).get('state') in TERMINAL)
        order = self._order(state, client)
        accounting_error = None
        if order and positive(order.get('order_id')):
            if observer is None:
                observer = self.observer_factory(offset, client, {order['order_id']},
                        self.scope.isin_id, quantity, lifenum)
            def original_accounted(now):
                observer.update()
                value = self._order(now, client)
                return value and value.get('state') in TERMINAL and observer.proved(value.get('executed'))
            try:
                self._wait(original_accounted)
            except HarnessStop as error:
                # Accounting failure never permits another natural attempt.
                # Current complete POS may still permit risk reduction below.
                self.cli.uncertain = True
                accounting_error = error
        elif self.cli.uncertain:
            # A current flat snapshot cannot prove that an uncertain Add never
            # reached the exchange. Do not manufacture cancellation evidence.
            raise HarnessStop('partial_uncertain_add_terminal_identity_unproved')
        # Each additional IOC is permitted only after its predecessor is known
        # terminal, current complete POS is authoritative, and deal/credit proof
        # agrees. Unknown outcomes stop; no command is resent.
        for flatten_index in range(4):
            state = self.cli.snapshot(entry=False)
            if any(x.get('pending_fill_reservations', {}).get('buy_quantity', 0) or
                   x.get('pending_fill_reservations', {}).get('sell_quantity', 0)
                   for x in state['instruments']):
                state, _ = self._wait(lambda now: all(
                    x['pending_fill_reservations']['buy_quantity'] ==
                    x['pending_fill_reservations']['sell_quantity'] == 0
                    for x in now['instruments']))
            require(not any(x.get('state') not in TERMINAL for x in state.get('orders', [])),
                    'partial_cleanup_live_order_unproved')
            positions = [x['xpos'] for x in state.get('positions', [])
                         if x.get('isin_id') == self.scope.isin_id]
            position = positions[0] if positions else 0
            if position == 0:
                self.cli.snapshot(require_flat=True, entry=False)
                verify_independent_flat(self.scope, self.independent_flat(), self.cli.clock_ns(),
                                        self.cli.last_command_utc_ns, lifenum)
                if accounting_error:
                    raise accounting_error
                return
            require(flatten_index < 3, 'partial_bounded_flattening_unproved')
            state = self.cli.snapshot()
            target = next(x for x in state['instruments'] if x['isin_id'] == self.scope.isin_id)
            side = 'sell' if position > 0 else 'buy'
            price = target['book']['best_bid' if side == 'sell' else 'best_ask']['price_scaled']
            quantity = min(abs(position), self.scope.quantity_limit,
                           self.scope.fixed_ask_budget_scaled // price)
            require(positive(quantity), 'partial_no_bounded_flatten_quantity')
            flat_client = f'q-partial-flat-{attempt}-{flatten_index}'
            offset = self.journal_offset()
            self._pace()
            self.cli.flatten(flat_client, side, quantity, price)
            state, order = self._wait(lambda s: self._order(s, flat_client)
                                     if positive((self._order(s, flat_client) or {}).get('order_id')) else None)
            observer = self.observer_factory(offset, flat_client, {order['order_id']},
                    self.scope.isin_id, quantity, state['trade_replay']['accepted_lifenum'], side=side)
            def accounted(now):
                observer.update()
                value = self._order(now, flat_client)
                return value and value.get('state') in TERMINAL and observer.proved(value.get('executed'))
            self._wait(accounted)
        raise HarnessStop('partial_bounded_flattening_unproved')

    def run(self, client_prefix='q-partial'):
        require(client_prefix == 'q-partial', 'partial_client_prefix_outside_scope')
        require(not self.started, 'partial_controller_is_one_shot')
        self.started = True
        self.deadline = min(self.monotonic() + self.scope.timeout_seconds,
                            self.cli.entry_deadline)
        self.cli.snapshot(require_flat=True)
        verify_independent_flat(self.scope, self.independent_flat(), self.cli.clock_ns())
        result = {'status': 'NOT_OBSERVED', 'reason': 'eligible_quote_not_observed', 'attempts': 0}
        while self.monotonic() < self.deadline and result['attempts'] < self.scope.max_attempts:
            state = self.cli.snapshot(require_flat=True)
            candidate = choose_candidate(self.scope, state, self.cli.read_proof(), self.cli.clock_ns())
            if candidate is None:
                self.sleep(.2)
                continue
            # No attempt/ID is consumed until all final pre-Add guards agree.
            state = self.cli.snapshot(require_flat=True)
            if choose_candidate(self.scope, state, self.cli.read_proof(), self.cli.clock_ns()) != candidate:
                continue
            if self.monotonic() >= self.deadline:
                break
            result['attempts'] += 1
            client = f'q-partial-{result["attempts"]}'
            offset = self.journal_offset()
            result['add_offset'] = offset
            primary = None
            observer = None
            try:
                self._pace()
                self.cli.place(client, candidate)
                state, order = self._wait(lambda s: self._order(s, client)
                        if positive((self._order(s, client) or {}).get('order_id')) else None)
                observer = self.observer_factory(offset, client, {order['order_id']},
                        self.scope.isin_id, candidate.quantity, candidate.trade_lifenum)
                partial = None
                while self.monotonic() < self.deadline:
                    observer.update()
                    state = self.cli.snapshot()
                    current = self._order(state, client)
                    if current and current.get('executed') == candidate.quantity:
                        # Credits may append between the observer read and the
                        # status read. Wait boundedly for the actual committed
                        # proof rather than misclassifying that normal race.
                        def fully_accounted(now):
                            observer.update()
                            return observer.proved(candidate.quantity)
                        self._wait(fully_accounted)
                        result['reason'] = 'all_lots_filled_before_move'
                        break
                    partial = self._partial(state, client, observer, candidate.quantity)
                    if partial:
                        break
                    if current and current.get('state') in TERMINAL:
                        def terminal_accounted(now):
                            observer.update()
                            row = self._order(now, client)
                            return row and observer.proved(row.get('executed'))
                        self._wait(terminal_accounted)
                        result['reason'] = 'zero_or_nonpartial_terminal_execution'
                        break
                    self.sleep(.02)
                if not partial:
                    if result['reason'] == 'eligible_quote_not_observed':
                        result['reason'] = 'committed_partial_not_observed'
                    continue
                self._pace()
                state = self.cli.snapshot()
                observer.update()
                fresh = self._partial(state, client, observer, candidate.quantity)
                if not fresh or fresh['order_id'] != partial['order_id'] or fresh['executed'] != partial['executed']:
                    result['reason'] = 'second_fill_raced_before_move'
                    continue
                if self.monotonic() >= self.deadline:
                    result['reason'] = 'partial_deadline_before_move'
                    continue
                price = passive_move_price(self.scope, state, self.cli.read_proof(), self.cli.clock_ns())
                move_offset, old_id = self.journal_offset(), fresh['order_id']
                try:
                    self.cli.move(client, candidate.quantity, price,
                                  expected_order_id=fresh['order_id'],
                                  expected_executed=fresh['executed'])
                except HarnessStop as error:
                    if str(error) != 'partial_move_snapshot_changed':
                        raise
                    result['reason'] = 'second_fill_raced_before_move'
                    continue
                def moved(now):
                    value = self._order(now, client)
                    matched = (value and positive(value.get('order_id')) and
                            value['order_id'] != old_id and value.get('quantity') == candidate.quantity and
                            value.get('executed') >= fresh['executed'] and
                            value.get('remaining') == candidate.quantity-value['executed'] and
                            observer.move_proved(move_offset, old_id, value['order_id']))
                    if not matched:
                        return None
                    # Bind a replacement only after its matching regime-3
                    # reply AND later TRADE commit. Then accept its own deals,
                    # including ones appended in that same read batch.
                    observer.order_ids.add(value['order_id'])
                    observer.update()
                    return value if observer.proved(value['executed']) else None
                _, replacement = self._wait(moved)
                result.update(status='PASS', reason='committed_partial_before_regime_three_move',
                              quantity=candidate.quantity, executed_before_move=fresh['executed'],
                              remaining_before_move=candidate.quantity-fresh['executed'],
                              original_order_id=old_id, replacement_order_id=replacement['order_id'],
                              move_offset=move_offset)
            except Exception as error:
                primary = error
                raise
            finally:
                try:
                    self._cleanup(client, result['attempts'], observer, offset,
                                  candidate.quantity, candidate.trade_lifenum)
                except Exception as cleanup_error:
                    # Preserve initiating failure separately from risk cleanup.
                    self.record('partial_cleanup_failed', {'initiating_code':
                        str(primary) if isinstance(primary, HarnessStop) else type(primary).__name__,
                        'cleanup_code': str(cleanup_error) if isinstance(cleanup_error, HarnessStop)
                        else type(cleanup_error).__name__})
                    raise
            if result['status'] == 'PASS':
                break
        result['end_offset'] = self.journal_offset()
        self.record('partial_result', result)
        return result
