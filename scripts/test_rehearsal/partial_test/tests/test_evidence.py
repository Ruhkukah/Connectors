"""Read actual NDJSON cursor/observer, including incomplete transactions and races."""
from pathlib import Path
import json
import tempfile
import unittest

from ..evidence import GeneralCommittedDealObserver
from ..scope import HarnessStop
from .fixtures import ISIN, LIFENUM, append_deal, append_event


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'interaction.ndjson'
        self.path.touch()
        self.client, self.order = 'q-partial-offline', 501

    def observer(self, quantity=8, offset=0):
        return GeneralCommittedDealObserver(self.path, offset, self.client,
                                            {self.order}, ISIN, quantity, LIFENUM)

    def deal(self, quantity=7, **kwargs):
        append_deal(self.path, client=self.client, order_id=self.order,
                    quantity=quantity, **kwargs)

    def test_seven_of_eight_committed_and_once_credited_is_partial(self):
        observe = self.observer()
        self.deal()
        observe.update()
        self.assertTrue(observe.proved(7))
        self.assertTrue(observe.partial(7))

    def test_one_of_two_uses_identical_observer(self):
        observe = self.observer(quantity=2)
        self.deal(quantity=1)
        observe.update()
        self.assertTrue(observe.partial(1))

    def test_zero_and_full_execution_never_prove_partial(self):
        observe = self.observer()
        self.assertFalse(observe.partial(0))
        self.deal(quantity=8)
        observe.update()
        self.assertTrue(observe.proved(8))
        self.assertFalse(observe.partial(8))

    def test_credit_before_commit_does_not_prove_partial(self):
        observe = self.observer()
        self.deal(commit=False)
        observe.update()
        self.assertFalse(observe.partial(7))
        append_event(self.path, 'transaction_commit', dict(stream='FORTS_TRADE_REPL'))
        observe.update()
        self.assertTrue(observe.partial(7))

    def test_commit_without_matching_credit_does_not_prove_partial(self):
        observe = self.observer()
        self.deal(credit=False)
        observe.update()
        self.assertFalse(observe.partial(7))

    def test_duplicate_credit_is_fatal_not_an_additional_execution(self):
        observe = self.observer()
        self.deal(duplicate=True)
        with self.assertRaisesRegex(HarnessStop, 'partial_credit_not_once'):
            observe.update()

    def test_missing_replication_action_is_not_assumed_to_be_insert(self):
        observe = self.observer()
        self.deal()
        events = [json.loads(line) for line in self.path.read_text().splitlines()]
        for event in events:
            if event['event'] == 'stream_row':
                event['data']['fields'] = [field for field in event['data']['fields']
                                            if field['name'] != 'replAct']
        self.path.write_text(''.join(json.dumps(event)+'\n' for event in events))
        with self.assertRaises(HarnessStop):
            observe.update()

    def test_wrong_instrument_or_old_precommand_deal_cannot_trigger(self):
        self.deal()
        observe = self.observer(offset=self.path.stat().st_size)
        append_deal(self.path, client=self.client, order_id=self.order,
                    quantity=7, isin=999, deal=902)
        observe.update()
        self.assertFalse(observe.partial(7))

    def test_two_lots_in_one_transaction_are_full_before_move(self):
        observe = self.observer(quantity=2)
        self.deal(quantity=2)
        observe.update()
        self.assertFalse(observe.partial(1))
        self.assertFalse(observe.partial(2))
        self.assertTrue(observe.proved(2))

    def test_later_fill_changes_partial_total_before_move(self):
        observe = self.observer()
        self.deal()
        observe.update()
        self.assertTrue(observe.partial(7))
        self.deal(quantity=1, deal=902, revision=102)
        observe.update()
        self.assertFalse(observe.partial(7))
        self.assertFalse(observe.partial(8))

    def test_history_gap_reopen_and_lifenum_change_invalidate_observation(self):
        for event, data in (('private_history_gap', {}), ('link_lost', {}),
                            ('listener_close', dict(stream='FORTS_TRADE_REPL')),
                            ('lifenum', dict(stream='FORTS_TRADE_REPL', value=32))):
            with self.subTest(event=event):
                self.path.write_text('')
                observe = self.observer()
                self.deal()
                append_event(self.path, event, data)
                with self.assertRaises(HarnessStop):
                    observe.update()

    def test_incomplete_last_line_waits_for_full_record(self):
        observe = self.observer()
        self.deal()
        with self.path.open('ab') as target:
            target.write(b'{"event":"trade","data":')
        observe.update()
        self.assertTrue(observe.partial(7))

    def test_move_needs_matching_code_zero_reply_and_later_trade_commit(self):
        observe = self.observer()
        offset = self.path.stat().st_size
        append_event(self.path, 'command', dict(
            name='MoveOrder', client_order_id=self.client, user_id=77,
            fields=dict(order_id1=self.order, regime=3)))
        append_event(self.path, 'reply', dict(user_id=77, msgid=176, code=0, order_id1=601))
        self.assertFalse(observe.move_proved(offset, self.order, 601))
        append_event(self.path, 'transaction_commit', dict(stream='FORTS_POS_REPL'))
        self.assertFalse(observe.move_proved(offset, self.order, 601))
        append_event(self.path, 'transaction_commit', dict(stream='FORTS_TRADE_REPL'))
        self.assertTrue(observe.move_proved(offset, self.order, 601))
        self.assertFalse(observe.move_proved(offset, self.order, 602))


if __name__ == '__main__':
    unittest.main()
