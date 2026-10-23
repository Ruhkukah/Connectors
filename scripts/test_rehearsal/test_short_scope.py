"""The declared date/window is shared without native or clock mutation."""
import datetime as dt
import unittest
import short_scope as scope


class ScopeTests(unittest.TestCase):
    def test_friday_uses_entire_owner_bounded_window_after_dsd_auction(self):
        cfg = scope.expected('2026-10-16')
        self.assertEqual(scope.window(cfg)['earliest_entry_utc'], scope.epoch('2026-10-16', 10))
        self.assertEqual(cfg['admission_latest_utc'], scope.epoch('2026-10-16', 12))
        self.assertEqual(cfg['hard_entry_deadline_utc'], scope.epoch('2026-10-16', 14, 30))
        self.assertEqual(cfg['flat_deadline_utc'], scope.epoch('2026-10-16', 14, 40))
        self.assertEqual(cfg['guardian_trigger_utc'], scope.epoch('2026-10-16', 14, 55))
        self.assertEqual(cfg['hard_cleanup_deadline_utc'], scope.epoch('2026-10-16', 15))

    def test_monday_fallback_is_a_separate_calendar_authorization(self):
        cfg = scope.expected('2026-10-19')
        self.assertEqual(scope.window(cfg)['earliest_entry_utc'], scope.epoch('2026-10-19', 9))
        self.assertEqual(cfg['product_start_latest_utc'], scope.epoch('2026-10-19', 9, 30))
        self.assertEqual(dt.datetime.fromtimestamp(cfg['hard_cleanup_deadline_utc'], scope.MSK).date().isoformat(), '2026-10-19')

    def test_window_mutation_bool_missing_and_later_date_refused(self):
        for key in scope.expected('2026-10-16'):
            cfg = scope.expected('2026-10-16')
            cfg.pop(key)
            with self.assertRaises(ValueError):
                scope.window(cfg)
        for value in (True, scope.epoch('2026-10-16', 11)):
            cfg = scope.expected('2026-10-16'); cfg['hard_cleanup_deadline_utc'] = value
            with self.assertRaisesRegex(ValueError, 'sealed_window'):
                scope.window(cfg)
        with self.assertRaisesRegex(ValueError, 'calendar_date'):
            scope.expected('2026-10-20')

    def continuation(self, start):
        return dict(scope.expected('2026-10-16',continuation_start_utc=start),
                    same_day_continuation=True,continuation_start_utc=start,
                    owner_authorized=True,predecessor_blocked_preentry_verified=True)

    def test_explicit_friday_continuation_keeps_all_admission_and_closeout_limits(self):
        start=scope.epoch('2026-10-16',10,20)
        cfg=self.continuation(start)
        result=scope.window(cfg)
        self.assertEqual(result['scheduled_start_utc'],start)
        self.assertEqual(result['product_start_latest_utc'],start+1800)
        original=scope.expected('2026-10-16')
        for key in set(original)-{'scheduled_start_utc','product_start_latest_utc'}:
            self.assertEqual(result[key],original[key])

    def test_latest_continuation_caps_product_start_before_noon_admission(self):
        cfg=self.continuation(scope.epoch('2026-10-16',11,30))
        result=scope.window(cfg)
        self.assertEqual(result['product_start_latest_utc'],scope.epoch('2026-10-16',11,50))
        self.assertEqual(result['admission_latest_utc'],scope.epoch('2026-10-16',12))

    def test_burst_only_continuation_accepts_owner_and_clean_terminal_proof(self):
        cfg=self.continuation(scope.epoch('2026-10-16',10,45))
        cfg.pop('predecessor_blocked_preentry_verified')
        cfg.update(burst_only=True,predecessor_clean_terminal_verified=True)
        result=scope.window(cfg)
        self.assertEqual(result['scheduled_start_utc'],scope.epoch('2026-10-16',10,45))
        self.assertEqual(result['hard_entry_deadline_utc'],scope.epoch('2026-10-16',14,30))

    def test_burst_only_continuation_refuses_missing_false_or_nonboolean_clean_proof(self):
        cfg=self.continuation(scope.epoch('2026-10-16',10,45))
        cfg['burst_only']=True
        for value in (None,False,1,'true'):
            with self.subTest(value=value),self.assertRaisesRegex(ValueError,'continuation_authority'):
                scope.window(dict(cfg,predecessor_clean_terminal_verified=value))
        with self.assertRaisesRegex(ValueError,'continuation_authority'):
            scope.window(dict(cfg,predecessor_clean_terminal_verified=True,owner_authorized=False))

    def test_clean_terminal_proof_alone_cannot_authorize_full_case_continuation(self):
        cfg=self.continuation(scope.epoch('2026-10-16',10,45))
        cfg.pop('predecessor_blocked_preentry_verified')
        cfg['predecessor_clean_terminal_verified']=True
        for value in (False,1,'true',None):
            with self.subTest(value=value),self.assertRaisesRegex(ValueError,'continuation_authority'):
                scope.window(dict(cfg,burst_only=value))

    def test_continuation_refuses_missing_owner_or_preentry_proof_and_unflagged_start(self):
        cfg=self.continuation(scope.epoch('2026-10-16',10))
        for key,value in [('owner_authorized',False),('predecessor_blocked_preentry_verified',False),
                          ('same_day_continuation',False),('same_day_continuation',1)]:
            with self.subTest(key=key,value=value),self.assertRaisesRegex(ValueError,'continuation_authority'):
                scope.window(dict(cfg,**{key:value}))

    def test_continuation_refuses_boolean_nonfinite_late_start_or_different_date(self):
        for value in (True,False,float('nan'),float('inf'),float('-inf'),10**1000,
                      scope.epoch('2026-10-16',8,59),scope.epoch('2026-10-16',11,31)):
            with self.subTest(value=value),self.assertRaisesRegex(ValueError,'continuation_start'):
                scope.expected('2026-10-16',continuation_start_utc=value)
        with self.assertRaisesRegex(ValueError,'continuation_start'):
            scope.expected('2026-10-19',continuation_start_utc=scope.epoch('2026-10-19',10))
        cfg=self.continuation(scope.epoch('2026-10-16',10))
        with self.assertRaisesRegex(ValueError,'sealed_window'):
            scope.window(dict(cfg,hard_cleanup_deadline_utc=scope.epoch('2026-10-16',16)))


if __name__ == '__main__':
    unittest.main()
