"""Exact frozen guardian composed with the real short supervisor startup methods.

Only clock, process receipts and file I/O are offline. No native process starts.
"""
import hashlib
import importlib.machinery
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import short_scope

FIXTURE = Path(__file__).with_name('fixtures') / 'startup_guardian.py.txt'
GUARDIAN_SHA = 'eaa71cd3e6df99fad377e5664673a704ad62ccd1b4964d687076192c834796b7'
assert hashlib.sha256(FIXTURE.read_bytes()).hexdigest() == GUARDIAN_SHA
loader = importlib.machinery.SourceFileLoader('_startup_exact_guardian', str(FIXTURE))
spec = importlib.util.spec_from_loader(loader.name, loader)
guardian = importlib.util.module_from_spec(spec)
loader.exec_module(guardian)


class Refused(RuntimeError):
    pass


def require(value, code):
    if not value:
        raise Refused(code)


class NoLegacyStartup:
    def publish(self, *_):
        raise AssertionError('startup must not fall through to the unleased legacy publisher')


base = types.ModuleType('base_supervisor')
base.Supervisor, base.Refused, base.require = NoLegacyStartup, Refused, require
base.public_value = lambda value: value
base.HEAD = '4ee96ce0cb41f0228fb74669f75d8be7a896c813'
base.BINARY = '3da6b0db4f36bbc53a9e173906315bc78cf55a7e2e7ee547bb7b9bdcedca27be'
spec = importlib.util.spec_from_file_location('_startup_real_supervisor', Path(__file__).with_name('short_supervisor.py'))
supervisor = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {'base_supervisor': base}):
    spec.loader.exec_module(supervisor)


class CompositionTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.now = short_scope.epoch('2026-10-16', 9) - .1
        self.runner = supervisor.Supervisor.__new__(supervisor.Supervisor)
        self.runner.root = self.root
        self.runner.cfg = short_scope.expected('2026-10-16')
        self.runner.public = dict(state='scheduled', phase='preflight', terminal=False)
        self.runner.startup_lease_active = True
        def atomic(path, value):
            Path(path).write_text(json.dumps(value))
        self.runner.rt = types.SimpleNamespace(atomic=atomic, sha=lambda _: 'boot', BOOT='boot')
        self.clock_patch = patch.object(supervisor.time, 'time', side_effect=lambda: self.now)
        self.clock_patch.start()
        self.addCleanup(self.clock_patch.stop)
        clock_ns = patch.object(supervisor.time, 'time_ns', side_effect=lambda: int(self.now*1e9))
        clock_ns.start()
        self.addCleanup(clock_ns.stop)

    def publish(self):
        self.runner.publish()
        return json.loads((self.root / 'public.json').read_text())

    def decision(self, public, alive=True):
        return guardian.decision(self.runner.cfg, public, self.now, alive, False, False)

    def test_exact_frozen_guardian_reproduces_original_0900_race(self):
        self.now = self.runner.cfg['scheduled_start_utc'] + .461
        old = dict(terminal=False, phase='prestart', updated_utc_ns=int(self.now*1e9))
        self.assertEqual(self.decision(old), 'BLOCKED_PRODUCT_ABSENT_WITHOUT_RESTART_LEASE')

    def test_guardian_first_at_boundary_and_delayed_router_check_keep_owner_lease(self):
        public = self.publish()
        self.assertEqual(self.decision(public), 'WAIT_PRESTART')
        # The guardian samples immediately after 09:00, before the supervisor
        # gets its first post-wake turn or finishes any router inspection.
        self.now = self.runner.cfg['scheduled_start_utc'] + .461
        self.assertEqual(self.decision(public), 'WAIT_LEASE')
        self.now += 20
        self.assertEqual(self.decision(public), 'WAIT_LEASE')
        self.assertFalse((self.root / 'active-product-private.json').exists())

    def test_late_install_starts_supervisor_receipt_before_guardian_without_product(self):
        self.now = short_scope.epoch('2026-10-16', 9, 52)
        public = self.publish()
        self.assertEqual(self.decision(public), 'WAIT_LEASE')
        self.assertLessEqual(public['ownership_lease_until_utc'], self.runner.cfg['product_start_latest_utc'])
        self.assertLessEqual(public['ownership_lease_until_utc'], self.now+100)

    def test_lease_cannot_extend_past_product_start_deadline(self):
        self.now = self.runner.cfg['product_start_latest_utc']-1
        public = self.publish()
        self.assertEqual(public['ownership_lease_until_utc'], self.runner.cfg['product_start_latest_utc'])
        self.assertEqual(self.decision(public), 'WAIT_LEASE')
        self.now += 2
        self.assertNotEqual(self.decision(public), 'WAIT_LEASE')

    def test_fresh_lease_does_not_excuse_dead_stale_or_expired_supervision(self):
        self.now = short_scope.epoch('2026-10-16', 9, 52)
        public = self.publish()
        self.assertEqual(self.decision(public, alive=False), 'PROTECT')
        stale = dict(public, updated_utc_ns=int((self.now-121)*1e9))
        self.assertEqual(self.decision(stale), 'PROTECT')
        self.now = public['ownership_lease_until_utc']+.1
        self.assertEqual(self.decision(public), 'BLOCKED_PRODUCT_ABSENT_WITHOUT_RESTART_LEASE')

    def sleep(self, seconds):
        self.now += max(.001, seconds)

    def test_late_install_actual_handshake_waits_until_guardian_sees_valid_supervisor_lease(self):
        self.now = short_scope.epoch('2026-10-16', 9, 52)
        self.publish()
        started = self.now
        guardian_decisions = []
        identity = dict(pid=22, start_ticks='2', executable='guardian-python')
        def guard_ok(root):
            receipt = json.loads((root/'guardian-owned.json').read_text())
            require(receipt == identity, 'wrong_guardian_identity')
        self.runner.rt.guard_ok = guard_ok
        self.runner.tick = lambda: self.fail('no native/runtime tick before guardian ownership handshake')
        def sleep(seconds):
            self.sleep(seconds)
            if self.now-started >= .1 and not (self.root/'guardian-owned.json').exists():
                public = json.loads((self.root/'public.json').read_text())
                guardian_decisions.append(self.decision(public))
                (self.root/'guardian-owned.json').write_text(json.dumps(identity))
        with patch.object(supervisor.time, 'sleep', side_effect=sleep):
            self.runner.await_startup_guardian()
        self.assertEqual(guardian_decisions, ['WAIT_LEASE'])
        self.assertFalse((self.root/'execution-claim.json').exists())
        self.assertFalse((self.root/'active-product-private.json').exists())

    def test_absent_guardian_or_arm_barrier_cannot_reach_native_preflight(self):
        self.now = short_scope.epoch('2026-10-16', 9, 52)
        self.runner.cfg['product_start_latest_utc'] = self.now+.15
        self.runner.rt.guard_ok = lambda _: (_ for _ in ()).throw(FileNotFoundError())
        with patch.object(supervisor.time, 'sleep', side_effect=self.sleep):
            with self.assertRaisesRegex(Refused, 'guardian_handshake_deadline'):
                self.runner.await_startup_guardian()
        self.runner.cfg.update(startup_arm_barrier_required=True, product_start_latest_utc=self.now+.15,
                               review_git_sha='sealed-review-head')
        self.runner.rt.guard_ok = lambda _: None
        with patch.object(supervisor.time, 'sleep', side_effect=self.sleep):
            with self.assertRaisesRegex(Refused, 'installation_barrier_deadline'):
                self.runner.await_installation_barrier()
        self.assertFalse((self.root/'execution-claim.json').exists())
        self.assertFalse((self.root/'active-product-private.json').exists())

    def test_exact_sealed_installation_barrier_releases_but_foreign_or_failed_does_not(self):
        self.now = short_scope.epoch('2026-10-16', 9, 52)
        self.runner.cfg.update(startup_arm_barrier_required=True, review_git_sha='sealed-review-head')
        self.runner.rt.guard_ok = lambda _: None
        self.runner.rt.sha = lambda path: 'boot' if str(path).startswith('/proc/') else 'sealed-manifest'
        receipt = dict(review_git_sha='sealed-review-head', authorized_date='2026-10-16', manifest_sha256='sealed-manifest')
        for key in receipt:
            path = self.root/'installation-complete.json'
            path.write_text(json.dumps(dict(receipt, **{key:'changed'})))
            with self.assertRaisesRegex(Refused, 'barrier_receipt_changed'):
                self.runner.await_installation_barrier()
        path.write_text(json.dumps(receipt))
        self.runner.await_installation_barrier()
        self.assertEqual(json.loads((self.root/'public.json').read_text())['startup_barrier'], 'RELEASED')
        (self.root/'guardian-done.json').write_text('{}')
        with self.assertRaisesRegex(Refused, 'guardian_already_terminal'):
            self.runner.await_installation_barrier()


if __name__ == '__main__':
    unittest.main()
