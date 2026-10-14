"""Admission evidence regressions; no venue clients, processes or network."""
import json
from pathlib import Path
import tempfile
import unittest

from native_admission import CommittedAdmissionObserver, INSTRUMENT, MAX_REVISION, SESSION
from native_evidence import EvidenceError


class NativeAdmissionTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.path = Path(temp.name) / 'journal'
        self.path.touch()
        self.observer = CommittedAdmissionObserver(self.path)

    def event(self, name, stream, **data):
        with self.path.open('a') as output:
            output.write(json.dumps(dict(event=name, data=dict(stream=stream, **data))) + '\n')

    def row(self, stream, table, repl_id, revision, deleted=0, **values):
        fields = dict(replID=repl_id, replRev=revision, replAct=deleted, **values)
        self.event('stream_row', stream, table=table, signed_value=revision,
                   fields=[dict(name=name, kind=1, signed=value) for name, value in fields.items()])

    def transaction(self, stream, callback):
        self.event('transaction_begin', stream)
        callback()
        self.event('transaction_commit', stream)

    def statuses(self, revision=2):
        for index, isin in enumerate((9, 10)):
            self.row(INSTRUMENT, 'instrument_state', 90+index, revision+index, isin_id=isin, public_state=1)

    def activate(self, marker=True, online=True):
        for stream in (SESSION, INSTRUMENT):
            self.event('listener_open', stream)
        self.transaction(SESSION, lambda: self.row(SESSION, 'session_state', 1, 1, sess_id=101, public_state=1))
        def instrument_snapshot():
            self.statuses()
            if marker:
                self.row(INSTRUMENT, 'sys_events', 100, 100, sess_id=101)
        self.transaction(INSTRUMENT, instrument_snapshot)
        if online:
            for stream in (SESSION, INSTRUMENT):
                self.event('listener_online', stream)
        self.observer.update()

    def admitted(self, session=101):
        self.observer.update()
        return self.observer.native_open(session, (9, 10))

    def test_committed_snapshot_order_and_optional_life_notices(self):
        self.activate()
        self.assertTrue(self.admitted())
        proof = self.observer.proof()
        self.assertIsNone(proof['streams'][INSTRUMENT]['lifenum'])
        self.assertEqual(proof['instrument_marker']['repl_id'], 100)

    def test_missing_marker_or_lifecycle_never_proves_authority(self):
        self.activate(marker=False)
        self.assertFalse(self.admitted())
        self.row(INSTRUMENT, 'sys_events', 100, 100, sess_id=101)
        with self.assertRaisesRegex(EvidenceError, 'without_native_transaction'):
            self.observer.update()

    def test_missing_open_or_online_cannot_prove_current_generation(self):
        self.activate(online=False)
        self.assertFalse(self.admitted())
        self.event('listener_online', SESSION)
        self.event('listener_online', INSTRUMENT)
        self.assertTrue(self.admitted())
        self.observer.streams[SESSION]['opened'] = False
        self.assertFalse(self.observer.native_open(101, (9, 10)))

    def test_raw_rows_and_open_transactions_are_not_admission(self):
        self.activate()
        self.event('transaction_begin', SESSION)
        self.row(SESSION, 'session_state', 1, 2, sess_id=101, public_state=0)
        self.assertFalse(self.admitted())
        self.assertEqual(self.observer.proof()['session_states'][101]['public_state'], 1)
        self.event('transaction_commit', SESSION)
        self.assertFalse(self.admitted())
        self.assertEqual(self.observer.proof()['session_states'][101]['public_state'], 0)

    def test_instrument_zero_is_not_open(self):
        self.activate()
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'instrument_state', 90, 4, isin_id=9, public_state=0))
        self.assertFalse(self.admitted())

    def test_older_marker_cannot_relabel_or_delete_latest(self):
        self.activate()
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 200, 90, sess_id=202))
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 100, 99, deleted=1))
        self.assertTrue(self.admitted())
        self.assertEqual(self.observer.proof()['instrument_marker']['repl_id'], 100)

    def test_unknown_and_nonlatest_sparse_tombstones_are_noops(self):
        self.activate()
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 101, 110, sess_id=101))
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 100, 120, deleted=1))
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 999, 130, deleted=1))
        self.assertTrue(self.admitted())
        self.assertEqual(self.observer.proof()['marker_highwater'], 110)

    def test_latest_sparse_tombstone_requires_new_marker_and_statuses(self):
        self.activate()
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 100, 110, deleted=1))
        self.assertFalse(self.admitted())
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 101, 111, sess_id=101))
        self.assertFalse(self.admitted())
        self.transaction(INSTRUMENT, lambda: self.statuses(112))
        self.assertTrue(self.admitted())

    def test_any_nonzero_signed_action_is_a_tombstone(self):
        self.activate()
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 100, 110, deleted=-1))
        self.assertFalse(self.admitted())
        self.assertFalse(self.observer.proof()['failed'])
        self.assertEqual(self.observer.proof()['marker_highwater'], 110)

    def test_online_session_change_requires_new_epoch_statuses(self):
        self.activate()
        self.transaction(SESSION, lambda: self.row(SESSION, 'session_state', 2, 2, sess_id=202, public_state=1))
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 101, 110, sess_id=202))
        self.assertFalse(self.admitted(202))
        self.transaction(INSTRUMENT, lambda: self.statuses(111))
        self.assertTrue(self.admitted(202))

    def test_snapshot_historical_markers_before_online_are_one_epoch(self):
        self.activate(online=False)
        before = self.observer.proof()['status_epoch']
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 101, 110, sess_id=202))
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 102, 120, sess_id=101))
        self.event('listener_online', SESSION)
        self.event('listener_online', INSTRUMENT)
        self.assertTrue(self.admitted())
        self.assertEqual(self.observer.proof()['status_epoch'], before)

    def test_scoped_purge_retains_floor_and_max_starts_revision_epoch(self):
        self.activate()
        self.event('clear_deleted', INSTRUMENT, table='unrelated', signed_value=MAX_REVISION)
        self.assertTrue(self.admitted())

        self.event('clear_deleted', INSTRUMENT, table='sys_events', signed_value=200)
        self.assertFalse(self.admitted())
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 101, 150, sess_id=101))
        self.assertFalse(self.admitted())
        self.assertIsNone(self.observer.proof()['instrument_marker'])
        self.event('clear_deleted', INSTRUMENT, table='sys_events', signed_value=MAX_REVISION)
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 102, 1, sess_id=101))
        self.transaction(INSTRUMENT, lambda: self.statuses(201))
        self.assertTrue(self.admitted())

    def test_unknown_purge_cannot_revive_older_marker_revision(self):
        self.activate()
        self.event('clear_deleted', INSTRUMENT, table='sys_events', signed_value=-1)
        self.assertFalse(self.admitted())
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 101, 90, sess_id=101))
        self.assertFalse(self.admitted())
        self.assertEqual(self.observer.proof()['marker_highwater'], 100)

    def test_actual_lifenum_and_close_reset_authority(self):
        self.activate()
        self.event('lifenum', INSTRUMENT, value=7)
        self.assertFalse(self.admitted())
        self.transaction(INSTRUMENT, lambda: (self.statuses(), self.row(INSTRUMENT, 'sys_events', 100, 100, sess_id=101)))
        self.event('listener_online', INSTRUMENT)
        self.assertTrue(self.admitted())
        self.assertEqual(self.observer.proof()['instrument_marker']['lifenum'], 7)
        self.event('listener_close', INSTRUMENT)
        self.assertFalse(self.admitted())

    def test_explicit_invalid_life_notice_cannot_become_optional_absence(self):
        for value in (0, None, '7', False):
            with self.subTest(value=value):
                self.path.write_text('')
                self.observer = CommittedAdmissionObserver(self.path)
                self.activate()
                self.event('lifenum', INSTRUMENT, value=value)
                with self.assertRaisesRegex(EvidenceError, 'lifenum_notice_invalid'):
                    self.observer.update()
                self.transaction(INSTRUMENT, lambda: (self.statuses(), self.row(INSTRUMENT, 'sys_events', 100, 100, sess_id=101)))
                self.event('listener_online', INSTRUMENT)
                self.assertFalse(self.admitted())
                self.assertTrue(self.observer.proof()['failed'])

    def test_repl_id_retarget_retires_old_key_even_with_newer_destination(self):
        self.activate()
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'instrument_state', 92, 100, isin_id=11, public_state=1))
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'instrument_state', 90, 50, isin_id=11, public_state=0))
        self.assertFalse(self.admitted())
        self.assertNotIn(9, self.observer.proof()['instrument_states'])
        self.transaction(SESSION, lambda: self.row(SESSION, 'session_state', 1, 3, sess_id=202, public_state=1))
        self.observer.update()
        self.assertNotIn(101, self.observer.proof()['session_states'])

    def test_orphan_commit_and_malformed_transaction_poison_admission(self):
        self.activate()
        self.event('transaction_commit', INSTRUMENT)
        with self.assertRaisesRegex(EvidenceError, 'commit_without_native_transaction'):
            self.observer.update()
        self.assertFalse(self.observer.native_open(101, (9, 10)))

    def test_malformed_later_row_cannot_publish_partial_authority(self):
        self.activate(marker=False)
        self.event('transaction_begin', INSTRUMENT)
        self.row(INSTRUMENT, 'sys_events', 100, 100, sess_id=101)
        self.row(INSTRUMENT, 'instrument_state', 0, 101, isin_id=9, public_state=1)
        self.event('transaction_commit', INSTRUMENT)
        with self.assertRaisesRegex(EvidenceError, 'service_identity_unproved'):
            self.observer.update()
        self.assertFalse(self.observer.native_open(101, (9, 10)))

    def test_public_proof_contains_only_numeric_native_provenance(self):
        self.activate()
        self.transaction(INSTRUMENT, lambda: self.row(INSTRUMENT, 'sys_events', 101, 110, sess_id=101, private_secret=1234567))
        self.observer.update()
        self.assertNotIn('private_secret', json.dumps(self.observer.proof()))


if __name__ == '__main__':
    unittest.main()
