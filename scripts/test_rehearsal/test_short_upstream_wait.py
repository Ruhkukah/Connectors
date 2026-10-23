"""Exercise the actual pre-entry wait, including failures before native intent."""
import ast
from pathlib import Path
import types
import unittest


class Refused(RuntimeError):
    pass


def require(value, code):
    if not value:
        raise Refused(code)


class UpstreamWaitTests(unittest.TestCase):
    def setUp(self):
        source = Path(__file__).with_name('short_supervisor.py').read_text()
        tree = ast.parse(source)
        cls = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'Supervisor')
        method = next(n for n in cls.body if isinstance(n, ast.FunctionDef) and n.name == 'await_preflight_upstreams')
        self.now = 0
        self.counts = [4]
        self.scans = self.ticks = 0
        self.published = []
        self.clients = 0
        self.whitelist_error = False
        self.unlisted_error = False
        self.change_identity = False
        self.scan_delay = 0
        self.tick_error = False
        clock = types.SimpleNamespace(time=lambda: self.now, sleep=self.sleep)
        namespace = dict(time=clock, base=types.SimpleNamespace(require=require, Refused=Refused))
        exec(compile(ast.Module(body=[method], type_ignores=[]), '<actual-upstream-wait>', 'exec'), namespace)
        launch = types.SimpleNamespace(pid=1, start=2, argv=(b'router',), env={}, cwd=Path('/test'))
        def inspect(_):
            self.scans += 1
            value = types.SimpleNamespace(**vars(launch))
            if self.change_identity and self.scans > 1:
                value.start += 1
            return value, dict(client_count=self.clients)
        def white(*_):
            if self.whitelist_error:
                raise Refused('pristine_whitelist_changed')
            return 'pinned'
        def rows(*_):
            self.now += self.scan_delay
            if self.unlisted_error:
                raise Refused('unlisted_owned_upstream')
            return list(range(self.counts[min(self.scans-1, len(self.counts)-1)]))
        probe = types.SimpleNamespace(inspect=inspect)
        upstream = types.SimpleNamespace(pristine_whitelist=white)
        guard = types.SimpleNamespace(strict_upstreams=rows)
        self.runner = types.SimpleNamespace(cfg=dict(product_start_latest_utc=3), tick=self.tick,
            rt=types.SimpleNamespace(_probe=lambda: (probe, guard, upstream), WHITE='pin'),
            publish=self.published.append)
        self.run_wait = lambda: namespace['await_preflight_upstreams'](self.runner)

    def sleep(self, seconds):
        self.now += seconds

    def tick(self):
        self.ticks += 1
        if self.tick_error:
            raise Refused('boot_or_guardian_or_stop_failed')

    def test_four_links_pass_immediately(self):
        self.run_wait()
        self.assertEqual(self.scans, 1)
        self.assertEqual(self.now, 0)

    def test_transient_zero_then_partial_then_four_waits_without_native_work(self):
        self.counts = [0, 3, 4]
        self.run_wait()
        self.assertEqual(self.scans, 3)
        self.assertEqual(self.ticks, 3)
        self.assertEqual([p['upstream_connections'] for p in self.published], [0, 3, 4])
        self.assertTrue(all(p['phase'] == 'preflight' for p in self.published))

    def test_permanent_zero_refuses_at_original_deadline(self):
        self.counts = [0]
        with self.assertRaisesRegex(Refused, 'upstreams_deadline'):
            self.run_wait()
        self.assertEqual(self.now, 3)
        self.assertEqual(self.scans, 3)

    def test_scan_completing_after_deadline_cannot_grant_entry(self):
        self.scan_delay = 4
        with self.assertRaisesRegex(Refused, 'upstreams_deadline'):
            self.run_wait()
        self.assertEqual(self.published, [])

    def test_changed_router_identity_refuses_without_retry(self):
        self.counts = [0, 4]
        self.change_identity = True
        with self.assertRaisesRegex(Refused, 'router_identity_changed'):
            self.run_wait()
        self.assertEqual(self.scans, 2)

    def test_competing_client_refuses(self):
        self.clients = 1
        with self.assertRaisesRegex(Refused, 'competing_client'):
            self.run_wait()
        self.assertEqual(self.scans, 1)

    def test_whitelist_failure_is_not_retried(self):
        self.whitelist_error = True
        with self.assertRaisesRegex(Refused, 'whitelist_changed'):
            self.run_wait()
        self.assertEqual(self.scans, 1)

    def test_unlisted_socket_failure_is_not_retried(self):
        self.unlisted_error = True
        with self.assertRaisesRegex(Refused, 'unlisted_owned_upstream'):
            self.run_wait()
        self.assertEqual(self.scans, 1)

    def test_extra_upstream_refuses(self):
        self.counts = [5]
        with self.assertRaisesRegex(Refused, 'upstream_count_invalid'):
            self.run_wait()

    def test_guardian_stop_or_boot_failure_is_not_retried(self):
        self.tick_error = True
        with self.assertRaisesRegex(Refused, 'boot_or_guardian_or_stop_failed'):
            self.run_wait()
        self.assertEqual(self.scans, 0)

    def test_execution_claim_and_product_start_follow_wait(self):
        source = Path(__file__).with_name('short_supervisor.py').read_text()
        tree = ast.parse(source)
        cls = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'Supervisor')
        run = ast.get_source_segment(source, next(n for n in cls.body if isinstance(n, ast.FunctionDef) and n.name == 'run'))
        self.assertLess(run.index('self.await_preflight_upstreams()'), run.index("'execution-claim.json'"))
        self.assertLess(run.index("'execution-claim.json'"), run.index('self.start()'))


if __name__ == '__main__':
    unittest.main()
