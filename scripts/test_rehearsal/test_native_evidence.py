"""Native evidence contracts: no venue commands, processes or network."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from native_evidence import CompactReplayVerifier, CommittedDealObserver, EvidenceError, JournalCursor, TRADE


class NativeEvidenceTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.path = Path(tmp.name) / 'journal'
        self.path.touch()
        streams = ['FORTS_TRADE_REPL', 'FORTS_USERORDERBOOK_REPL', 'FORTS_POS_REPL', 'FORTS_PART_REPL',
                   'FORTS_REFDATA_REPL', 'FORTS_SESSIONSTATE_REPL', 'FORTS_INSTRUMENTSTATE_REPL', 'FORTS_AGGR20_REPL', 'p2mqreply']
        self.anchor = dict(trades_rev=100, trades_lifenum=7)
        self.state = dict(reconstructing=False, queued=0,
                          trade_replay=dict(anchor_ready=True, accepted_lifenum=7, used_anchor=self.anchor),
                          pos_anchor=dict(trades_rev=100, trades_lifenum=7, online=True, snapshot_complete=True),
                          transport=dict(valid=True, connection=dict(state=3), listeners=[dict(stream=s, online=True, snapshot_complete=True, native=dict(state=3), error=dict(code=0)) for s in streams]))

    def event(self, name, **data):
        data.setdefault('stream', TRADE)
        with self.path.open('a') as f:
            f.write(json.dumps(dict(event=name, data=data)) + '\n')

    def row(self, deal=1, quantity=1, revision=101, **extra):
        values = dict(isin_id=9, public_order_id_buy=10, private_order_id_buy=0, id_deal=deal, xamount=quantity, replRev=revision, replAct=0)
        values.update(extra)
        self.event('stream_row', table='user_deal', fields=[dict(name=k, signed=v) for k, v in values.items()])

    def credit(self, deal=1, quantity=1, **extra):
        values = dict(client_order_id='own', order_id=10, deal_id=deal, quantity=quantity)
        values.update(extra)
        self.event('trade', stream=None, **values)

    def observer(self):
        return CommittedDealObserver(self.path, 0, 'own', {10}, 9)

    def replay(self, revision=101, reason='minute', commit=True, opened=True):
        if opened:
            self.event('listener_open')
        self.event('transaction_begin')
        self.row(revision=102)
        self.event('trade_server_time', repl_rev=revision, server_time=999999, reason=reason)
        if commit:
            self.event('transaction_commit')
        self.event('listener_online')
        v = CompactReplayVerifier(self.path, 0)
        v.update()
        return v

    def test_compact_revision_committed_and_bound_to_pos_proves_replay(self):
        v = self.replay()
        self.assertEqual(v.result(self.state, self.anchor), dict(proven=True, first_replay={'user_deal':102, 'heartbeat':101}))

    def test_server_time_is_not_revision(self):
        # Invalid committed native revisions refuse, rather than using server time.
        with self.assertRaisesRegex(EvidenceError, 'replay_revision_missing'):
            self.replay(revision=None)

    def test_below_floor_revision_refuses(self):
        v = self.replay(revision=99)
        with self.assertRaisesRegex(EvidenceError, 'below_pos_anchor'):
            v.result(self.state, self.anchor)

    def test_no_open_or_no_commit_cannot_prove_replay(self):
        for kwargs in (dict(opened=False), dict(commit=False)):
            self.path.write_text('')
            v = self.replay(**kwargs)
            self.assertFalse(v.result(self.state, self.anchor)['proven'])

    def test_lifecycle_sample_cannot_substitute_for_fresh_heartbeat(self):
        v = self.replay(reason='open')
        self.assertFalse(v.result(self.state, self.anchor)['proven'])

    def test_reopen_discards_previous_replay_proof(self):
        v = self.replay()
        self.event('listener_open')
        v.update()
        self.assertFalse(v.result(self.state, self.anchor)['proven'])

    def test_clear_deleted_and_lifenum_invalidate_replay(self):
        for reset in ('clear_deleted', 'lifenum', 'listener_close'):
            self.path.write_text('')
            v = self.replay()
            self.event(reset, value=8)
            v.update()
            self.assertFalse(v.result(self.state, self.anchor)['proven'])

    def test_saved_or_current_lifenum_mismatch_cannot_prove(self):
        v = self.replay()
        self.assertFalse(v.result(self.state, dict(trades_rev=100, trades_lifenum=8))['proven'])
        state = copy.deepcopy(self.state)
        state['trade_replay']['accepted_lifenum'] = 8
        self.assertFalse(v.result(state, self.anchor)['proven'])

    def test_incomplete_reconstruction_alert_and_missing_stream_refuse(self):
        v = self.replay()
        states = []
        for key,value in [('reconstructing',True),('queued',1),('journal_failed',True)]:
            s = copy.deepcopy(self.state);s[key]=value;states.append(s)
        s = copy.deepcopy(self.state);s['pos_anchor']['snapshot_complete']=False;states.append(s)
        s = copy.deepcopy(self.state);s['transport']['listeners'].pop();states.append(s)
        s = copy.deepcopy(self.state);s['pos_anchor']['trades_rev']=99;states.append(s)
        for s in states:
            self.assertFalse(v.result(s, self.anchor)['proven'])

    def test_offset_excludes_pre_restart_proof(self):
        self.replay()
        v = CompactReplayVerifier(self.path, self.path.stat().st_size)
        v.update()
        self.assertFalse(v.result(self.state, self.anchor)['proven'])

    def test_aborted_transaction_cannot_supply_replay_rows(self):
        v = self.replay(commit=False)
        self.event('transaction_begin');self.event('transaction_commit')
        v.update()
        self.assertFalse(v.result(self.state, self.anchor)['proven'])

    def test_committed_one_lot_needs_matching_once_credit(self):
        o=self.observer();self.event('transaction_begin');self.row();o.update()
        self.assertFalse(o.proved(1))
        self.event('transaction_commit');o.update();self.assertFalse(o.proved(1))
        self.credit();o.update();self.assertTrue(o.proved(1))
        offset=o.cursor.offset;o.update();self.assertEqual(o.cursor.offset,offset);self.assertTrue(o.proved(1))

    def test_native_two_lot_deal_is_full_not_partial(self):
        o=self.observer();self.event('transaction_begin');self.row(quantity=2);self.event('transaction_commit');self.credit(quantity=2);o.update()
        self.assertFalse(o.proved(1));self.assertTrue(o.proved(2))

    def test_same_commit_two_one_lot_deals_never_partial_even_if_second_credit_delayed(self):
        o=self.observer();self.event('transaction_begin');self.row();self.row(deal=2);self.event('transaction_commit');self.credit();o.update()
        self.assertFalse(o.proved(1))
        self.credit(deal=2);o.update();self.assertTrue(o.proved(2))

    def test_second_commit_invalidates_partial_before_status_catches_up(self):
        o=self.observer();self.event('transaction_begin');self.row();self.event('transaction_commit');self.credit();o.update();self.assertTrue(o.proved(1))
        self.event('transaction_begin');self.row(deal=2);self.event('transaction_commit');o.update();self.assertFalse(o.proved(1))

    def test_duplicate_credit_and_quantity_rewrite_refuse(self):
        o=self.observer();self.event('transaction_begin');self.row();self.event('transaction_commit');self.credit();o.update()
        self.credit()
        with self.assertRaisesRegex(EvidenceError,'fill_credit_not_once'):o.update()
        self.path.write_text('');o=self.observer();self.event('transaction_begin');self.row();self.event('transaction_commit');o.update()
        self.event('transaction_begin');self.row(quantity=2)
        with self.assertRaisesRegex(EvidenceError,'quantity_changed'):o.update()

    def test_wrong_instrument_order_and_deleted_row_not_proof(self):
        for overrides in (dict(isin_id=8),dict(public_order_id_buy=11),dict(replAct=1)):
            self.path.write_text('');o=self.observer();self.event('transaction_begin');self.row(**overrides);self.event('transaction_commit');self.credit();o.update();self.assertFalse(o.proved(1))

    def test_resets_discard_partial_proof_and_aborted_rows(self):
        for reset in ('listener_open','listener_close','lifenum','clear_deleted'):
            self.path.write_text('');o=self.observer();self.event('transaction_begin');self.row();self.event('transaction_commit');self.credit();o.update();self.assertTrue(o.proved(1))
            self.event(reset);o.update();self.assertFalse(o.proved(1))
        self.path.write_text('');o=self.observer();self.event('transaction_begin');self.row();self.event('transaction_begin');self.event('transaction_commit');self.credit();o.update();self.assertFalse(o.proved(1))

    def test_wrong_credit_join_and_missing_native_quantity_cannot_prove(self):
        o=self.observer();self.event('transaction_begin');self.row();self.event('transaction_commit');self.credit(deal=2);o.update();self.assertFalse(o.proved(1))
        self.path.write_text('');o=self.observer();self.event('transaction_begin');self.row(quantity=True)
        with self.assertRaisesRegex(EvidenceError,'quantity_or_identity_invalid'):o.update()

    def test_post_restart_uses_compact_proof_and_retains_legacy_missing_heartbeat(self):
        # Exercise the real sequence integration through the supplied fake CLI.
        import sequence
        from unittest.mock import patch
        checkpoint=dict(records=[], trade_replay=dict(used_anchor=self.anchor), journal_offset=0)
        (self.path.parent/'sequence-restart-private.json').write_text(json.dumps(checkpoint))
        self.replay()
        state=copy.deepcopy(self.state);state['orders']=[]
        class Fake:
            def __init__(inner,*args):
                inner.work=self.path.parent;inner.journal=self.path;inner.targets=[]
            def snapshot(inner):return state
            def wait(inner,predicate,code):
                self.assertTrue(predicate(state));return state,True
            def assert_flat(inner,current):pass
        with patch.object(sequence,'_Sequence',Fake):
            result=sequence.post_restart(None,None,self.path.parent,[])
        self.assertTrue(result['restart_replay_floor_proven'])
        self.assertFalse(result['restart_legacy_replay_floor_proven'])
        self.assertEqual(result['restart_missing_replay_tables'],['heartbeat'])
        self.assertEqual(result['restart_compact_replay']['first_replay']['heartbeat'],101)

    def test_table_scoped_purge_retains_only_revisions_at_or_above_floor(self):
        v=self.replay()
        self.event('clear_deleted',table='orders_log',signed_value=999)
        self.event('clear_deleted',table='heartbeat',signed_value=101)
        self.event('clear_deleted',table='user_deal',signed_value=102)
        v.update();self.assertTrue(v.result(self.state,self.anchor)['proven'])
        self.event('clear_deleted',table='heartbeat',signed_value=102)
        v.update();self.assertFalse(v.result(self.state,self.anchor)['proven'])
        self.event('transaction_begin');self.event('trade_server_time',repl_rev=101,reason='minute');self.event('transaction_commit')
        v.update();self.assertFalse(v.result(self.state,self.anchor)['proven'])

    def test_max_purge_requires_fresh_epoch_proof(self):
        v=self.replay();self.event('clear_deleted',table='heartbeat',signed_value=9223372036854775807)
        v.update();self.assertFalse(v.result(self.state,self.anchor)['proven'])

    def test_deal_scoped_purge_cannot_reuse_removed_or_stale_row(self):
        o=self.observer();self.event('transaction_begin');self.row();self.event('transaction_commit');self.credit();o.update()
        self.event('clear_deleted',table='orders_log',signed_value=999);o.update();self.assertTrue(o.proved(1))
        self.event('clear_deleted',table='user_deal',signed_value=101);o.update();self.assertTrue(o.proved(1))
        self.event('clear_deleted',table='user_deal',signed_value=102);o.update();self.assertFalse(o.proved(1))
        self.event('transaction_begin');self.row();self.event('transaction_commit');o.update();self.assertFalse(o.proved(1))

    def test_partial_line_waits_and_invalid_complete_line_refuses(self):
        cursor=JournalCursor(self.path,0)
        self.path.write_bytes(b'{"event":"transaction_begin","data":{}')
        self.assertEqual(list(cursor.events()),[]);self.assertEqual(cursor.offset,0)
        with self.path.open('ab') as f:f.write(b'}\n')
        self.assertEqual(len(list(cursor.events())),1)
        with self.path.open('ab') as f:f.write(b'invalid\n')
        with self.assertRaisesRegex(EvidenceError,'invalid_record'):list(cursor.events())

    def test_truncated_and_replaced_journal_refuse(self):
        self.event('listener_open');cursor=JournalCursor(self.path,0);list(cursor.events());self.path.write_text('')
        with self.assertRaisesRegex(EvidenceError,'truncated'):list(cursor.events())
        cursor=JournalCursor(self.path,0);replacement=self.path.with_suffix('.new');replacement.write_text('');replacement.replace(self.path)
        with self.assertRaisesRegex(EvidenceError,'replaced'):list(cursor.events())
