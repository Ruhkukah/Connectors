"""Report-only correction: positive evening and final-clearing evidence coexist."""
import copy
import unittest
from session_coverage import evaluate_session_coverage, SESSION, TRADE, PART


class SessionCoverageTests(unittest.TestCase):
    def setUp(self):
        self.evening = dict(status='PASS', commands=['AddDay', 'Move', 'Del'],
                            symbols=['ALRS-12.26', 'CNY-12.26'], journal_begin=10, journal_end=20)
        self.clearing = dict(status='PASS', same_product_observed_clearing=True,
                             native_clearing_utc=100, journal_begin=30, journal_end=100,
                             committed_session_transitions=[self.row(SESSION, 'session_state', 35,
                                                                     public_state=4)],
                             committed_system_events=[self.row(stream, 'sys_events', offset,
                                 event_id=501, event_type=5, server_time=100)
                                 for stream, offset in ((TRADE, 40), (PART, 50))])
        self.epochs = {SESSION: 31, TRADE: 41, PART: 51}

    @staticmethod
    def row(stream, table, offset, **fields):
        return dict(stream=stream, table=table, row_offset=offset, commit_offset=offset+1,
                    committed_utc=100.1, native_fields=dict(sess_id=11729, replAct=0,
                                                         replRev=offset, **fields))

    def result(self, **changes):
        args = dict(session_id=11729, source_epochs=self.epochs,
                    observed_epochs=copy.deepcopy(self.epochs), same_process_verified=True)
        args.update(changes)
        return evaluate_session_coverage(self.evening, self.clearing, **args)

    def test_evening_before_final_clearing_is_positive_coverage(self):
        value = self.result()
        self.assertEqual(value['status'], 'PASS')
        self.assertFalse(value['evening_after_final_clearing_required'])
        self.assertTrue(value['overall_qualification_unchanged'])

    def test_input_receipts_are_not_rewritten(self):
        original = copy.deepcopy((self.evening, self.clearing))
        self.result()
        self.assertEqual(original, (self.evening, self.clearing))

    def test_evening_schedule_alone_does_not_replace_actual_commands(self):
        self.evening['status'] = 'NOT_OBSERVED'
        self.assertEqual(self.result()['status'], 'INCOMPLETE')
        self.evening['status'] = 'PASS'
        self.evening['commands'].remove('Move')
        self.assertEqual(self.result()['evening_commands'], 'INCOMPLETE')

    def test_different_process_or_epoch_never_passes(self):
        self.assertEqual(self.result(same_process_verified=False)['status'], 'INCOMPLETE')
        self.assertEqual(self.result(observed_epochs={**self.epochs, TRADE: 42})['status'], 'INCOMPLETE')

    def test_missing_epoch_is_not_inferred(self):
        self.assertEqual(self.result(observed_epochs={})['status'], 'INCOMPLETE')

    def test_wrong_session_or_deleted_native_row_does_not_pass(self):
        for field, value in (('sess_id', 11728), ('replAct', 1)):
            with self.subTest(field=field):
                original = copy.deepcopy(self.clearing)
                self.clearing['committed_session_transitions'][0]['native_fields'][field] = value
                self.assertEqual(self.result()['final_clearing'], 'INCOMPLETE')
                self.clearing = original

    def test_row_without_later_commit_does_not_pass(self):
        row = self.clearing['committed_session_transitions'][0]
        row['commit_offset'] = row['row_offset']
        self.assertEqual(self.result()['final_clearing'], 'INCOMPLETE')

    def test_commit_or_exchange_event_before_boundary_does_not_pass(self):
        self.clearing['committed_session_transitions'][0]['committed_utc'] = 99.9
        self.assertEqual(self.result()['final_clearing'], 'INCOMPLETE')
        self.clearing['committed_session_transitions'][0]['committed_utc'] = 100.1
        self.clearing['committed_system_events'][1]['native_fields']['server_time'] = 99
        self.assertEqual(self.result()['final_clearing'], 'INCOMPLETE')

    def test_trade_and_part_have_to_corroborate_same_event(self):
        self.clearing['committed_system_events'][1]['native_fields']['event_id'] = 502
        self.assertEqual(self.result()['final_clearing'], 'INCOMPLETE')

    def test_incomplete_range_or_missing_stream_does_not_pass(self):
        self.clearing['journal_end'] = 30
        self.assertEqual(self.result()['final_clearing'], 'INCOMPLETE')
        self.clearing['journal_end'] = 100
        self.clearing['committed_system_events'].pop()
        self.assertEqual(self.result()['final_clearing'], 'INCOMPLETE')


if __name__ == '__main__':
    unittest.main()
