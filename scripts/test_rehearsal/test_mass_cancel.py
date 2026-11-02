"""Mass cancellation requires a real acknowledgement or a fresh native empty proof."""
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import sequence
import ops

class MassCancelProof(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.s=object.__new__(sequence._Sequence)
        self.s.journal=Path(self.tmp.name)/'interaction.ndjson';self.s.journal.touch()
        self.s.targets=[dict(isin_id=1,symbol='ALRS-12.26')]
        self.state=dict(queued=0,orders=[])
        self.events=[]
        self.s.send=lambda _:self.write(self.events)
        self.s.wait=self.wait
    def write(self,events):
        with self.s.journal.open('a') as f:
            for event,data in events:f.write(json.dumps(dict(event=event,data=data))+'\n')
    def wait(self,predicate,reason,timeout):
        if not predicate(self.state):raise sequence.SequenceStop(reason)
    def empty(self,isin=1,confirmed=True):
        return ('cancel_all_empty',dict(isin_id=isin,snapshot_confirmed=confirmed))
    def run_proof(self,ready=True):
        with patch.object(ops,'ready',return_value=ready) as gate:
            self.s.mass_cancel(self.s.targets[0])
            return gate
    def test_fresh_native_empty_proof_with_current_reconstruction_passes(self):
        self.events=[self.empty()];gate=self.run_proof();gate.assert_called_once_with(self.state,self.s.targets,entry=False)
    def test_old_empty_proof_is_not_reused(self):
        self.write([self.empty()])
        with self.assertRaises(sequence.SequenceStop):self.run_proof()
    def test_wrong_scope_or_unconfirmed_empty_proof_refused(self):
        for event in [self.empty(2),self.empty(confirmed=False)]:
            self.events=[event]
            with self.assertRaises(sequence.SequenceStop):self.run_proof()
    def test_empty_proof_under_incomplete_reconstruction_refused(self):
        self.events=[self.empty()]
        with self.assertRaises(sequence.SequenceStop):self.run_proof(ready=False)
    def test_working_or_queued_orders_refuse_empty_proof(self):
        self.events=[self.empty()]
        for state in [dict(queued=1,orders=[]),dict(queued=0,orders=[dict(state='Working')])]:
            self.state=state
            with self.assertRaises(sequence.SequenceStop):self.run_proof()
    def test_real_mass_cancel_requires_matching_reply_and_later_commit(self):
        command=('command',dict(name='DelUserOrders',fields=dict(isin_id=1),user_id=5))
        reply=('reply',dict(user_id=5,msgid=186,code=0))
        commit=('transaction_commit',dict(stream='FORTS_TRADE_REPL'))
        for events in [[command],[command,reply],[commit,command,reply],[command,('reply',dict(user_id=6,msgid=186,code=0)),commit]]:
            self.events=events
            with self.assertRaises(sequence.SequenceStop):self.run_proof()
        self.events=[command,reply,commit];self.run_proof()

if __name__=='__main__':unittest.main()
