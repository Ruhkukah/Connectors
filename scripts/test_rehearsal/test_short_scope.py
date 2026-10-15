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


if __name__ == '__main__':
    unittest.main()
