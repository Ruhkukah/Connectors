"""Typed command boundary for an eventual protected actual-moexctl adapter.

There is no subprocess, socket, SDK, launcher or deployment code here. The
injected invoke must run actual ``moexctl plaza2 cmd`` with the returned single
command argument, using the protected socket/credentials. Never execute a shell.
"""
import re
import time
from .scope import (HarnessStop, Candidate, TERMINAL, WORKING, choose_candidate,
                    positive, require, validate_runtime)


def price_text(value):
    return f'{value // 100000}.{value % 100000:05d}'.rstrip('0').rstrip('.')


class GuardedCli:
    def __init__(self, scope, invoke, snapshot, proof, clock_ns=time.time_ns,
                 monotonic=time.monotonic):
        self.scope, self.invoke, self.read_status, self.read_proof = scope, invoke, snapshot, proof
        self.clock_ns, self.monotonic = clock_ns, monotonic
        self.last_command = None
        self.uncertain = False
        self.intents = {}
        self.entry_attempts = 0
        self.entry_deadline = self.monotonic() + self.scope.timeout_seconds
        self.last_command_utc_ns = 0

    def snapshot(self, require_flat=False, entry=True):
        try:
            state, proof = self.read_status(), self.read_proof()
        except Exception as error:
            raise HarnessStop('partial_snapshot_or_identity_uncertain') from error
        validate_runtime(self.scope, state, proof, self.clock_ns(), require_flat, entry)
        return state

    def _send(self, line, exposure=False):
        require(not exposure or not self.uncertain, 'partial_uncertain_add_never_retried')
        now = self.monotonic()
        require(self.last_command is None or now - self.last_command >= .2,
                'partial_rate_limit')
        self.last_command = now
        self.last_command_utc_ns = self.clock_ns()
        try:
            result = self.invoke(('plaza2', 'cmd', line))
        except Exception as error:
            self.uncertain = True
            raise HarnessStop('partial_command_outcome_uncertain') from error
        if not isinstance(result, dict) or result.get('ok') is not True:
            self.uncertain = True
            raise HarnessStop('partial_command_refused_or_uncertain')
        return result

    def place(self, client, candidate):
        require(isinstance(client, str) and re.fullmatch(r'q-partial-[A-Za-z0-9_-]{1,80}', client),
                'partial_client_identity_invalid')
        require(client not in self.intents and not self.uncertain,
                'partial_uncertain_add_never_retried')
        require(self.entry_attempts < self.scope.max_attempts and
                self.monotonic() < self.entry_deadline, 'partial_entry_attempt_or_time_budget')
        state = self.snapshot(require_flat=True)
        require(isinstance(candidate, Candidate) and
                choose_candidate(self.scope, state, self.read_proof(), self.clock_ns()) == candidate,
                'partial_candidate_changed_before_add')
        # Durable reservation remains the product's responsibility. Set intent
        # before transport: even a timeout never permits this Add to be retried.
        self.intents[client] = candidate.quantity
        self.entry_attempts += 1
        return self._send(f'place {client} {self.scope.isin_id} buy {candidate.quantity} '
                          f'{price_text(candidate.price_scaled)} day', exposure=True)

    def _owned(self, state, client):
        rows = [x for x in state.get('orders', []) if x.get('client_order_id') == client]
        require(client in self.intents and len(rows) == 1, 'partial_owned_order_missing_or_ambiguous')
        row = rows[0]
        require(row.get('instance_owned') is True and row.get('isin_id') == self.scope.isin_id and
                row.get('operator_action_required') is False and row.get('state') in WORKING and
                positive(row.get('order_id')), 'partial_order_identity_unproved')
        return row

    def move(self, client, quantity, price_scaled, *, expected_order_id, expected_executed):
        require(not self.uncertain, 'partial_uncertain_command_blocks_move')
        state = self.snapshot()
        row = self._owned(state, client)
        require(row.get('order_id') == expected_order_id and
                row.get('executed') == expected_executed,
                'partial_move_snapshot_changed')
        require(type(quantity) is int and quantity == self.intents[client] == row.get('quantity') and
                type(row.get('executed')) is int and 0 < row['executed'] < quantity and
                row.get('remaining') == quantity - row['executed'] and
                row.get('execution_baseline_known') is True,
                'partial_move_original_total_or_partial_unproved')
        instrument = next(x for x in state['instruments'] if x['isin_id'] == self.scope.isin_id)
        terms, book = instrument['session_terms'], instrument['book']
        require(positive(price_scaled) and price_scaled % terms['min_step_scaled'] == 0 and
                terms['lower_price_scaled'] <= price_scaled <= terms['upper_price_scaled'] and
                price_scaled <= book['best_bid']['price_scaled'] and
                price_scaled < book['best_ask']['price_scaled'] and
                quantity * price_scaled <= self.scope.fixed_ask_budget_scaled and
                quantity * price_scaled <= quantity * book['best_ask']['price_scaled'],
                'partial_move_price_or_notional_outside_scope')
        return self._send(f'move {client} {quantity} {price_text(price_scaled)}')

    def cancel(self, client):
        state = self.snapshot(entry=False)
        self._owned(state, client)
        return self._send(f'cancel {client}')

    def flatten(self, client, side, quantity, price_scaled):
        """Exact current exposure only. Never cross through zero or use Day."""
        require(isinstance(client, str) and re.fullmatch(r'q-partial-flat-[A-Za-z0-9_-]{1,80}', client),
                'partial_flatten_client_invalid')
        require(client not in self.intents, 'partial_uncertain_add_never_retried')
        state = self.snapshot()
        require(not any(x.get('state') not in TERMINAL for x in state.get('orders', [])),
                'partial_flatten_with_live_order_refused')
        positions = [x['xpos'] for x in state.get('positions', []) if x.get('isin_id') == self.scope.isin_id]
        position = positions[0] if positions else 0
        require(positive(quantity) and quantity <= abs(position) <= self.scope.position_limit and
                quantity <= self.scope.quantity_limit and
                side == ('sell' if position > 0 else 'buy'), 'partial_flatten_not_risk_reducing')
        row = next(x for x in state['instruments'] if x['isin_id'] == self.scope.isin_id)
        level = row['book']['best_bid' if side == 'sell' else 'best_ask']
        require(price_scaled == level['price_scaled'] and
                quantity * price_scaled <= self.scope.fixed_ask_budget_scaled,
                'partial_flatten_price_or_notional_outside_scope')
        self.intents[client] = quantity
        # This is a new, uniquely identified risk-reducing IOC, never a replay
        # of an uncertain Add. Product ownership and reconciliation must be known.
        return self._send(f'place {client} {self.scope.isin_id} {side} {quantity} '
                          f'{price_text(price_scaled)} ioc')
