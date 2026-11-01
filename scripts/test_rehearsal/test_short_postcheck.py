import datetime as dt
import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import Mock

import short_postcheck as check


class PostcheckTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        day = dt.datetime(2026, 10, 16, tzinfo=dt.timezone(dt.timedelta(hours=3)))
        self.cfg = dict(authorized_date='2026-10-16',
            post_session_check_utc=day.replace(hour=15, minute=10).timestamp(),
            post_session_check_deadline_utc=day.replace(hour=23, minute=40).timestamp())
        self.now = lambda: self.cfg['post_session_check_utc']
        self.meta = dict(identity=dict(pid=123), state='live.state')
        self.inventory = dict(client_count=0, upstreams=4)
        self.proof = dict(complete_replication_streams=8, working_orders=0,
            nonzero_positions=0, exit=0, publisher_posts=0, message_allocations=0,
            checkpoint_unchanged_by_probe=True, after=self.inventory)
        self.runtime = types.SimpleNamespace(inventory=Mock(return_value=self.inventory),
            sha=Mock(return_value='unchanged'), readonly_flat=Mock(return_value=self.proof))
        self.ops = types.SimpleNamespace(process_identity=Mock(side_effect=FileNotFoundError))

    def run_check(self):
        return check.check_once(self.root, self.cfg, self.runtime, self.ops, self.meta, self.now)

    def test_independent_check_is_one_publisher_free_observation(self):
        result = self.run_check()
        self.assertEqual(result['state'], 'INDEPENDENT_FLAT_CONFIRMED')
        self.assertEqual(result['trading_commands'], 0)
        self.runtime.readonly_flat.assert_called_once()

    def test_alive_product_or_foreign_client_prevents_probe(self):
        self.ops.process_identity.side_effect = None
        self.ops.process_identity.return_value = self.meta['identity']
        with self.assertRaisesRegex(check.PostcheckRefused, 'still_alive'):
            self.run_check()
        self.ops.process_identity.side_effect = FileNotFoundError
        self.runtime.inventory.return_value = dict(client_count=1, upstreams=4)
        with self.assertRaisesRegex(check.PostcheckRefused, 'idle_router'):
            self.run_check()
        self.runtime.readonly_flat.assert_not_called()

    def test_unattached_child_receipt_blocks_probe_despite_stale_active_metadata(self):
        (self.root / 'active-product-private.json').write_text(json.dumps(dict(identity=dict(pid=111))))
        (self.root / 'emergency-product-owned-private.json').write_text(json.dumps(dict(pid=222)))
        self.ops.process_identity.side_effect = lambda pid: dict(pid=222) if pid == 222 else dict(pid=999)
        with self.assertRaisesRegex(check.PostcheckRefused, 'still_alive'):
            check.no_owned_products(self.root, self.ops)

    def test_later_date_or_wrong_window_never_starts_a_probe(self):
        self.now = lambda: self.cfg['post_session_check_deadline_utc'] + 1
        with self.assertRaisesRegex(check.PostcheckRefused, 'same_day_window'):
            self.run_check()
        self.runtime.readonly_flat.assert_not_called()

    def test_incomplete_snapshot_or_bad_exit_is_not_flatness(self):
        for key, value in [('exit', 5), ('complete_replication_streams', 7),
                           ('working_orders', 1), ('message_allocations', 1)]:
            self.runtime.readonly_flat.return_value = dict(self.proof, **{key: value})
            with self.assertRaisesRegex(check.PostcheckRefused, 'flatness_unproved'):
                self.run_check()

    def test_complete_nonflat_snapshot_is_an_alarm_not_retried(self):
        folder = self.root / 'post-session-independent-flat'
        folder.mkdir()
        capture = dict(self.proof, exit=5, fresh_private_snapshot_ready=True,
                       stop_ok=True, working_orders=2)
        (folder / 'snapshot-observation-public.json').write_text(json.dumps(capture))
        self.runtime.readonly_flat.side_effect = RuntimeError('not flat')
        result = self.run_check()
        self.assertEqual(result['state'], 'NONFLAT_ACTION_REQUIRED')
        self.assertEqual(result['working_orders'], 2)
        self.assertFalse(result['automatic_recovery'])
        self.runtime.readonly_flat.assert_called_once()

    def test_unknown_exit_never_uses_a_nonflat_capture_as_complete_proof(self):
        folder = self.root / 'post-session-independent-flat'
        folder.mkdir()
        (folder / 'snapshot-observation-public.json').write_text(json.dumps(
            dict(self.proof, exit=None, working_orders=2)))
        self.runtime.readonly_flat.side_effect = RuntimeError('unknown child exit')
        with self.assertRaisesRegex(RuntimeError, 'unknown child exit'):
            self.run_check()

    def test_private_card_preserves_exact_quoted_launch_without_executing_it(self):
        (self.root / 'SHORT_CONTINGENCY.md').write_text('Operator guards\n')
        launch = types.SimpleNamespace(argv=[b'/router binary', b'arg with quote\''],
            env={b'KEY': b'private $value'}, cwd=Path('/private/router cwd'))
        path = check.prepare_private_card(self.root, launch)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        text = path.read_text()
        self.assertIn("cd -- '/private/router cwd'", text)
        self.assertIn("'KEY=private $value'", text)
        self.assertIn("'cancel-all '+str(t['isin_id'])", text)
        self.assertIn('prints commands only', text)

    def test_private_card_refuses_nonprivate_directory(self):
        self.root.chmod(0o755)
        with self.assertRaisesRegex(check.PostcheckRefused, 'private_owned'):
            check.prepare_private_card(self.root, types.SimpleNamespace())


if __name__ == '__main__':
    unittest.main()
