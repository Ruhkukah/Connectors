"""Offline supervisor tests: synthetic state only, no CGate or network."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('sequence', str(Path(__file__).with_name('sequence.py')))
sequence = importlib.util.module_from_spec(spec)
sys.modules['sequence'] = sequence
spec.loader.exec_module(sequence)
spec = importlib.util.spec_from_file_location('scenarios', str(Path(__file__).with_name('scenarios.py')))
scenarios = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scenarios)


class FakeSequence:
    def __init__(self, scenario='partial', commit=True, credit=True, duplicate=False):
        self.tmp = tempfile.TemporaryDirectory()
        self.journal = Path(self.tmp.name) / 'interaction.ndjson'
        self.journal.touch()
        self.targets = [{'symbol': 'CNY-12.26', 'isin_id': 1}, {'symbol': 'ALRS-12.26', 'isin_id': 2}]
        self.scenario, self.commit, self.credit, self.duplicate = scenario, commit, credit, duplicate
        self.commands, self.rows, self.records = [], {}, []
        self.counter = 0
        self.tick = 0
        self.fill_recorded = False
        self.state = dict(queued=0, reconstructing=False, positions=[], orders=[], configuration={'risk': {'max_open_orders': 5, 'instruments': [dict(isin_id=x, max_quote_notional_scaled=20000000, max_position=2) for x in (1, 2)]}}, instruments=[dict(isin_id=x, symbol=s, order_entry_ready=True, session_terms=dict(min_step_scaled=10000, reference_price_scaled=10000000, lower_price_scaled=8000000, upper_price_scaled=12000000), book=dict(valid=True, best_bid=dict(price_scaled=9900000), best_ask=dict(price_scaled=10000000))) for x, s in ((1, 'CNY-12.26'), (2, 'ALRS-12.26'))])

    def event(self, name, data):
        with self.journal.open('a') as out:
            out.write(json.dumps(dict(event=name, data=data)) + '\n')

    def record(self, name, value):
        self.records.append((name, value))

    def snapshot(self):
        self.tick += 1
        for row in self.rows.values():
            if row['quantity'] == 2 and row['state'] in sequence.WORKING and not self.fill_recorded:
                quantity = {'partial': 1, 'full': 2, 'none': 0}[self.scenario]
                row.update(executed=quantity, remaining=2-quantity, state='Filled' if quantity == 2 else 'PartFilled' if quantity else 'Working')
                if quantity:
                    self.fill_recorded = True
                    fields = dict(isin_id=1, replAct=0, public_order_id_buy=row['order_id'], private_order_id_buy=0, id_deal=901, xamount=quantity)
                    self.event('transaction_begin', dict(stream='FORTS_TRADE_REPL'))
                    self.event('stream_row', dict(stream='FORTS_TRADE_REPL', table='user_deal', fields=[dict(name=k, signed=v) for k, v in fields.items()]))
                    if self.commit:
                        self.event('transaction_commit', dict(stream='FORTS_TRADE_REPL'))
                    if self.credit:
                        trade = dict(client_order_id=row['client_order_id'], order_id=row['order_id'], deal_id=901, quantity=quantity)
                        self.event('trade', trade)
                        if self.duplicate:
                            self.event('trade', trade)
        self.state['orders'] = list(self.rows.values())
        return copy.deepcopy(self.state)

    def assert_flat(self, value):
        if any(x['state'] not in sequence.TERMINAL for x in value['orders']):
            raise sequence.SequenceStop('not_flat')

    def target(self, value, target, require_ready=True):
        return next(x for x in value['instruments'] if x['isin_id'] == target['isin_id'])

    def passive(self, value, target, quantity):
        return 8100000

    def valid_price(self, value, target, price, quantity):
        if not 8000000 <= price <= 12000000 or price % 10000 or quantity * price > 20000000:
            raise sequence.SequenceStop('price_outside_native_limits')

    def next_id(self, target):
        self.counter += 1
        return f'q-case-{self.counter}'

    def order(self, value, client):
        return next((x for x in value['orders'] if x['client_order_id'] == client), None)

    def send(self, line):
        self.commands.append(line)
        bits = line.split()
        if bits[0] == 'place':
            client, isin, quantity, price = bits[1], int(bits[2]), int(bits[4]), bits[5]
            self.rows[client] = dict(client_order_id=client, isin_id=isin, state='Working', order_id=100+self.counter, quantity=quantity, executed=0, remaining=quantity, price=price, instance_owned=True)
        elif bits[0] == 'move':
            row = self.rows[bits[1]]
            if row['executed'] != 1 or row['remaining'] != 1:
                raise AssertionError('Move ran before genuine partial fill')
            if bits[2] != '2':
                raise AssertionError('logical Move quantity changed')
            old = row['order_id']
            row.update(order_id=old+1000, price=bits[3])
            self.event('command', dict(name='MoveOrder', user_id=123, fields=dict(order_id1=old, regime=3)))
            self.event('reply', dict(user_id=123, msgid=176, code=0, order_id1=row['order_id']))
        elif bits[0] == 'cancel':
            self.rows[bits[1]].update(state='Cancelled', remaining=0)

    def accepted_order(self, target, client):
        return self.rows[client]

    def cancel(self, client, allowed_fill=0):
        self.send('cancel '+client)
        return copy.deepcopy(self.rows[client])

    def mass_cancel(self, target):
        self.commands.append('cancel-all '+str(target['isin_id']))
        for row in self.rows.values():
            if row['isin_id'] == target['isin_id']:
                row.update(state='Cancelled', remaining=0)

    def wait(self, predicate, code, timeout=60):
        for _ in range(5):
            state = self.snapshot()
            value = predicate(state)
            if value:
                return state, value
        raise sequence.SequenceStop(code)


class ScenariosTest(unittest.TestCase):
    def make(self, *args, **kwargs):
        seq = FakeSequence(*args, **kwargs)
        self.addCleanup(seq.tmp.cleanup)
        return seq

    def partial(self, seq):
        # Advance virtual observation time without waiting or sending anything.
        moments = iter(x / 10 for x in range(1000))
        with patch.object(scenarios.time, 'sleep', return_value=None), patch.object(scenarios.time, 'monotonic', side_effect=lambda: next(moments)):
            return scenarios.partial_move(seq, seq.targets[0], timeout_seconds=1)

    def test_real_committed_partial_moves_logical_two_then_cancels(self):
        seq = self.make()
        result = self.partial(seq)
        self.assertEqual(result['status'], 'PASS')
        self.assertTrue(result['partial_before_move'])
        self.assertEqual(result['native_regime'], 3)
        self.assertEqual(len([x for x in seq.commands if x.startswith('move ')]), 1)
        self.assertTrue(all(x['state'] in sequence.TERMINAL for x in seq.rows.values()))

    def test_full_fill_never_moves(self):
        seq = self.make('full')
        self.assertEqual(self.partial(seq)['reason'], 'fully_filled_before_move')
        self.assertFalse(any(x.startswith('move ') for x in seq.commands))

    def test_zero_fill_never_moves_and_cancels(self):
        seq = self.make('none')
        self.assertEqual(self.partial(seq)['status'], 'NOT_OBSERVED')
        self.assertFalse(any(x.startswith('move ') for x in seq.commands))
        self.assertTrue(any(x.startswith('cancel ') for x in seq.commands))

    def test_uncommitted_native_row_does_not_authorize_move(self):
        seq = self.make(commit=False)
        self.assertEqual(self.partial(seq)['status'], 'NOT_OBSERVED')
        self.assertFalse(any(x.startswith('move ') for x in seq.commands))
        self.assertEqual(next(iter(seq.rows.values()))['state'], 'Cancelled')

    def test_native_row_without_credited_trade_does_not_authorize_move(self):
        seq = self.make(credit=False)
        self.assertEqual(self.partial(seq)['status'], 'NOT_OBSERVED')
        self.assertFalse(any(x.startswith('move ') for x in seq.commands))

    def test_duplicate_credit_stops_and_cleans_known_remainder(self):
        seq = self.make(duplicate=True)
        with self.assertRaisesRegex(sequence.SequenceStop, 'fill_credit_not_once'):
            self.partial(seq)
        self.assertFalse(any(x.startswith('move ') for x in seq.commands))
        self.assertEqual(next(iter(seq.rows.values()))['state'], 'Cancelled')

    def native_proof(self, names):
        seq = self.make('none')
        fields = dict(isin_id=1, replAct=0, public_order_id_buy=101, private_order_id_buy=0, id_deal=901, xamount=1)
        for name in names:
            if name == 'row':
                seq.event('stream_row', dict(stream='FORTS_TRADE_REPL', table='user_deal', fields=[dict(name=k, signed=v) for k, v in fields.items()]))
            elif name == 'other_close':
                seq.event('listener_close', dict(stream='FORTS_POS_REPL'))
            else:
                seq.event(name, dict(stream='FORTS_TRADE_REPL', value=123))
        seq.event('trade', dict(client_order_id='q-proof', order_id=101, deal_id=901, quantity=1))
        return scenarios._credited_native(seq, 0, 'q-proof', {101}, 1, 1)

    def test_native_row_without_begin_cannot_use_commit(self):
        self.assertFalse(self.native_proof(['row', 'transaction_commit']))

    def test_new_begin_discards_aborted_transaction(self):
        self.assertFalse(self.native_proof(['transaction_begin', 'row', 'transaction_begin', 'transaction_commit']))

    def test_closed_listener_row_cannot_use_later_commit(self):
        self.assertFalse(self.native_proof(['transaction_begin', 'row', 'listener_close', 'transaction_begin', 'transaction_commit']))

    def test_lifenum_change_row_cannot_use_later_commit(self):
        self.assertFalse(self.native_proof(['transaction_begin', 'row', 'lifenum', 'transaction_begin', 'transaction_commit']))

    def test_clear_deleted_row_cannot_use_later_commit(self):
        self.assertFalse(self.native_proof(['transaction_begin', 'row', 'clear_deleted', 'transaction_begin', 'transaction_commit']))

    def test_reopened_listener_row_cannot_use_later_commit(self):
        self.assertFalse(self.native_proof(['transaction_begin', 'row', 'listener_open', 'transaction_begin', 'transaction_commit']))

    def test_closed_listener_commit_without_new_begin_is_not_proof(self):
        self.assertFalse(self.native_proof(['transaction_begin', 'row', 'listener_close', 'transaction_commit']))

    def test_matching_begin_row_commit_is_proof(self):
        self.assertTrue(self.native_proof(['transaction_begin', 'row', 'transaction_commit']))

    def test_other_stream_close_does_not_abort_trade_transaction(self):
        self.assertTrue(self.native_proof(['transaction_begin', 'row', 'other_close', 'transaction_commit']))

    def test_combined_quote_cap_prevents_any_concurrent_add(self):
        seq = self.make('none')
        seq.state['configuration']['risk']['instruments'][0]['max_quote_notional_scaled'] = 15000000
        with self.assertRaisesRegex(sequence.SequenceStop, 'concurrent_combined_quote_cap'):
            scenarios.concurrent_cancel(seq)
        self.assertEqual(seq.commands, [])

    def test_three_concurrent_then_individual_and_both_scopes(self):
        seq = self.make('none')
        result = scenarios.concurrent_cancel(seq)
        self.assertEqual(result['concurrent_orders'], 3)
        self.assertEqual(seq.commands[3].split()[0], 'cancel')
        self.assertEqual(seq.commands[4:], ['cancel-all 1', 'cancel-all 2'])
        self.assertEqual([x['isin_id'] for x in seq.rows.values()], [1,1,2])
        self.assertTrue(all(x['state'] in sequence.TERMINAL for x in seq.rows.values()))


if __name__ == '__main__':
    unittest.main()
