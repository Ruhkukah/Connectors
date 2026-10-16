import copy
import sys
import tempfile
import time
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).parent))
import price_caps,sequence

class CurrentPriceCaps(unittest.TestCase):
    def setUp(self):
        self.target=dict(isin_id=1,symbol='ALRS-12.26',reference_price_scaled=185800000)
        self.state=dict(sess_id=12,reconstructing=False,instruments=[dict(isin_id=1,symbol='ALRS-12.26',order_entry_ready=True,session_terms=dict(sess_id=12,refdata_lifenum=7,interval_valid=True,reference_price_scaled=185800000,min_step_scaled=100000,lower_price_scaled=167700000,upper_price_scaled=1012100000),book=dict(valid=True,stream_last_callback_utc_ns=time.time_ns(),best_bid=dict(price_scaled=779600000,quantity=10),best_ask=dict(price_scaled=802000000,quantity=1)))],configuration=dict(risk=dict(instruments=[dict(isin_id=1,max_position=2,max_quote_notional_scaled=1559200000)])))
    def test_current_price_not_settlement(self):
        t=price_caps.bind_caps(self.state,[self.target])[0]
        self.assertEqual(t['reference_price_scaled'],185800000)
        self.assertEqual(t['quote_reference_scaled'],779600000)
        self.assertEqual(t['quote_cap_scaled'],1559200000)
    def seq(self,target):
        tmp=tempfile.TemporaryDirectory();self.addCleanup(tmp.cleanup)
        s=object.__new__(sequence._Sequence);s.targets=[target];return s
    def test_one_current_ioc_fits_without_permitting_two_at_ask(self):
        t=price_caps.bind_caps(self.state,[self.target])[0];s=self.seq(t)
        s.valid_price(self.state,t,802000000,1)
        with self.assertRaisesRegex(sequence.SequenceStop,'quote_cap_exceeded'):s.valid_price(self.state,t,802000000,2)
    def test_native_bounds_still_enforced(self):
        t=price_caps.bind_caps(self.state,[self.target])[0]
        with self.assertRaisesRegex(sequence.SequenceStop,'price_outside_native_limits'):self.seq(t).valid_price(self.state,t,1020000000,1)
    def test_frozen_cap_never_increases(self):
        t=price_caps.bind_caps(self.state,[self.target])[0]
        self.state['configuration']['risk']['instruments'][0]['max_quote_notional_scaled']+=100000
        with self.assertRaisesRegex(sequence.SequenceStop,'approved_scope'):self.seq(t).valid_price(self.state,t,802000000,1)
    def test_stale_crossed_off_grid_wrong_session_missing_volume_refused(self):
        for kind in ('stale','crossed','grid','session','volume'):
            with self.subTest(kind=kind):
                state=copy.deepcopy(self.state);row=state['instruments'][0]
                if kind=='stale':row['book']['stream_last_callback_utc_ns']-=11000000000
                if kind=='crossed':row['book']['best_bid']['price_scaled']=803000000
                if kind=='grid':row['book']['best_ask']['price_scaled']+=1
                if kind=='session':row['session_terms']['sess_id']+=1
                if kind=='volume':row['book']['best_ask']['quantity']=0
                with self.assertRaises(ValueError):price_caps.bind_caps(state,[self.target])
    def test_quote_decline_does_not_expand_per_request_ceiling(self):
        t=price_caps.bind_caps(self.state,[self.target])[0]
        self.state['instruments'][0]['book']['best_bid']['price_scaled']=700000000
        with self.assertRaisesRegex(sequence.SequenceStop,'quote_cap_exceeded'):self.seq(t).valid_price(self.state,t,750000000,2)
    def test_decimal_units_are_identical(self):
        self.assertEqual(sequence._units('8020'),802000000)
        self.assertEqual(sequence._units('1858'),185800000)
        self.assertEqual(sequence._price(1559200000),'15592')

if __name__=='__main__':unittest.main()
