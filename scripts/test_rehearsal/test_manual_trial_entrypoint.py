import importlib.util
import contextlib
import io
import json
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock, patch
import fcntl

FILE = Path(__file__).with_name('manual_trial_entrypoint.py')


def load():
    spec = importlib.util.spec_from_file_location('manual_entrypoint_under_test', FILE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class EntrypointTests(unittest.TestCase):
    def setUp(self):
        self.m = load()
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        self.root.chmod(0o700)

    def tearDown(self):
        self.tmp.cleanup()

    def test_import_has_no_process_or_network_actions(self):
        with patch('subprocess.Popen', side_effect=AssertionError('Popen during import')), \
             patch('subprocess.run', side_effect=AssertionError('run during import')), \
             patch('os.kill', side_effect=AssertionError('signal during import')):
            load()

    def test_execute_flag_and_role_mandatory(self):
        for args in ([], ['--root', str(self.root), '--manifest-sha', 'a'*64, '--role', 'owner']):
            with patch.object(self.m, 'execute') as execute, contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                self.m.main(args)
            execute.assert_not_called()

    def test_changed_manifest_file_refuses_before_any_component_import(self):
        root = self.root/'moex/qualification/job'
        root.mkdir(parents=True, mode=0o700)
        runtime = root/'runtime.py'
        runtime.write_text('original')
        runtime.chmod(0o600)
        manifest = root/'manifest.json'
        manifest.write_text(json.dumps(dict(source_git_sha=self.m.SOURCE, binary_sha256=self.m.BINARY,
                                            files={'runtime.py': self.m.sha(runtime)})))
        manifest.chmod(0o600)
        pin = self.m.sha(manifest)
        runtime.write_text('changed-after-seal')
        with patch.object(self.m.Path, 'home', return_value=self.root), \
             patch.object(self.m.sys, 'platform', 'linux'), patch.object(self.m, 'setup') as setup:
            with self.assertRaisesRegex(self.m.Refused, 'sealed_file_changed'):
                self.m.execute(root, pin, 'owner')
        setup.assert_not_called()

    def test_runtime_scope_installed_before_load_and_canonical_helpers_only(self):
        events = []
        cfg = {'scope': 'test'}
        names = ('manual_trial_day', 'runtime', 'ops', 'supervisor', 'guardian',
                 'router_recovery', 'upstream_recovery', 'short_fault_helpers',
                 'base_supervisor', 'frozen_runtime')
        modules = {name: SimpleNamespace(__file__=str(self.root/(name+'.py'))) for name in names}
        modules['manual_trial_day'].install_runtime_scope = lambda rt: events.append('scope')
        modules['manual_trial_day'].validate = lambda got, **kw: self.assertIs(got, cfg)
        def load_cfg(root):
            self.assertEqual(events, ['scope'])
            events.append('load')
            return cfg
        modules['runtime'].load_cfg = load_cfg
        modules['supervisor'].load_fault_helpers = lambda *a: events.append('canonical_install')
        with patch.object(self.m.importlib, 'import_module', side_effect=lambda name: modules[name]):
            result = self.m.setup(self.root)
        self.assertEqual(events, ['scope', 'load', 'canonical_install'])
        self.assertIs(result['cfg'], cfg)

    def test_canonical_import_from_other_root_refuses(self):
        with patch.object(self.m.importlib, 'import_module', return_value=SimpleNamespace(__file__='/wrong/runtime.py')):
            with self.assertRaisesRegex(self.m.Refused, 'canonical_helper_import_mismatch'):
                self.m.setup(self.root)

    def parts(self, owner):
        return dict(runtime=Mock(), cfg={}, ops=Mock(), short=Mock(), guardian=Mock(),
                    manual=SimpleNamespace(owner_class=lambda short: lambda *a: owner))

    def test_owner_failure_calls_existing_fail_once_never_restarts(self):
        owner = Mock()
        original = RuntimeError('controlled native-owner failure')
        owner.run.side_effect = original
        parts = self.parts(owner)
        module = SimpleNamespace(__file__=str(self.root/'component.py'))
        with patch.object(self.m.sys, 'platform', 'linux'), \
             patch.object(self.m, 'sealed_root', return_value=self.root), \
             patch.object(self.m, 'setup', return_value=parts), \
             patch.object(self.m.importlib, 'import_module', return_value=module):
            self.assertEqual(self.m.execute(self.root, 'a'*64, 'owner'), 1)
        owner.run.assert_called_once_with()
        owner.fail.assert_called_once_with(original)
        owner.restart.assert_not_called()

    def test_supervisor_lock_prevents_second_owner(self):
        owner = Mock()
        with (self.root/'supervisor.lock').open('w') as lock:
            os.chmod(lock.name, 0o600)
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with patch.object(self.m.sys, 'platform', 'linux'), \
                 patch.object(self.m, 'sealed_root', return_value=self.root), \
                 patch.object(self.m, 'setup', return_value=self.parts(owner)):
                with self.assertRaises(BlockingIOError):
                    self.m.execute(self.root, 'a'*64, 'owner')
        owner.run.assert_not_called()

    def test_guardian_role_uses_direct_guardian_and_normal_terminal_proof(self):
        owner = Mock()
        parts = self.parts(owner)
        done = self.root/'guardian-done.json'
        done.write_text(json.dumps(dict(state='SUPERVISOR_TERMINAL_VERIFIED', no_guardian_failure=True)))
        done.chmod(0o600)
        with patch.object(self.m.sys, 'platform', 'linux'), \
             patch.object(self.m, 'sealed_root', return_value=self.root), \
             patch.object(self.m, 'setup', return_value=parts):
            self.assertEqual(self.m.execute(self.root, 'a'*64, 'guardian'), 0)
        parts['guardian'].Guardian.return_value.run.assert_called_once_with()
        owner.run.assert_not_called()

    def test_guardian_failure_receipt_cannot_return_success(self):
        parts = self.parts(Mock())
        done = self.root/'guardian-done.json'
        done.write_text(json.dumps(dict(state='BLOCKED', no_guardian_failure=False)))
        done.chmod(0o600)
        with patch.object(self.m.sys, 'platform', 'linux'), \
             patch.object(self.m, 'sealed_root', return_value=self.root), \
             patch.object(self.m, 'setup', return_value=parts):
            with self.assertRaisesRegex(self.m.Refused, 'guardian_did_not_verify_normal_terminal'):
                self.m.execute(self.root, 'a'*64, 'guardian')


if __name__ == '__main__':
    unittest.main()
