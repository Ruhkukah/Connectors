"""Real frozen helpers/core are imported offline; no native or network calls."""
import ast
import copy
import importlib.util
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import t0_composition as composition

HERE = Path(__file__).parent


class Composition(unittest.TestCase):
    def sources(self):
        result = {name + '.py': (HERE / 'fixtures' / (name + '.py.txt')).read_text()
                  for name in ('router_recovery', 'upstream_recovery', 'working_fault_core')}
        for name in ('short_fault_helpers.py', 'short_fault_core.py', 'short_session.py'):
            result[name] = (HERE / name).read_text()
        result['runtime.py'] = ('"""held runtime"""\nfrom __future__ import annotations\n'
            'REVIEW_ONLY = True\ndef load_cfg(root):\n    return root.read_text()\n'
            'def main():\n    raise AssertionError("native action")\n')
        result['guardian.py'] = ('def main():\n    assert cfg["authorized_date"] == "2026-10-19"\n'
            'class Guardian:\n    def run(self):\n        raise AssertionError("native action")\n')
        result['short_postcheck.py'] = (HERE / 'short_postcheck.py').read_text()
        return result

    def config(self):
        return dict(source_git_sha=composition.SOURCE, binary_sha256=composition.BINARY,
            target_venue='T0', authorized_date='2026-10-21', router_port=4101,
            router_profile_sha256=composition.T0_PROFILE, quantity_limit=2, position_limit=2,
            open_order_limit=5, rate_limit=5, partial_fill_enabled=False, timer_paused=True,
            owner_authorized=False, review_only=True, armed=False, allow_orders=False,
            assignment='/private/t0/PROPOSED/assignment.json',
            state='/private/t0/PROPOSED/checkpoint')

    def composed(self):
        return composition.compose(self.sources(), self.config(), {'/private/t0/PROPOSED': '/private/t0/registered'})

    def import_module(self, name, text):
        module = types.ModuleType(name)
        module.__file__ = '<held-offline-' + name + '>'
        sys.modules[name] = module
        exec(compile(text, module.__file__, 'exec'), module.__dict__)
        return module

    def modules(self, copies):
        names = ('router_recovery', 'upstream_recovery', 'working_fault_core',
                 'short_fault_helpers', 'short_fault_core', 'short_scope', 'short_session', 'day_driver')
        self.old = {name: sys.modules.get(name) for name in names}
        def restore():
            for name, value in self.old.items():
                if value is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = value
        self.addCleanup(restore)
        # Only the historical driver's base is absent from these public fixtures.
        # Core/helpers below are the actual reviewed source, not fake replicas.
        base = types.ModuleType('day_driver')
        base.DayDriver = object
        base.DayStop = RuntimeError
        base.ops = base.sequence = base.price_caps = types.SimpleNamespace()
        sys.modules['day_driver'] = base
        sys.modules['short_scope'] = types.ModuleType('short_scope')
        result = {}
        with patch.object(subprocess, 'Popen', side_effect=AssertionError('native process prohibited')), \
             patch.object(socket, 'socket', side_effect=AssertionError('socket prohibited')):
            for name in ('router_recovery', 'upstream_recovery', 'working_fault_core',
                         'short_fault_helpers', 'short_fault_core', 'short_session'):
                result[name] = self.import_module(name, copies[name + '.py'])
        return result

    def test_inputs_unchanged_and_config_held_after_registration_relocation(self):
        sources, cfg = self.sources(), self.config()
        before = copy.deepcopy((sources, cfg))
        copies, result, report = composition.compose(sources, cfg, {'/private/t0/PROPOSED': '/private/t0/registered'})
        self.assertEqual((sources, cfg), before)
        self.assertEqual(result['assignment'], '/private/t0/registered/assignment.json')
        self.assertEqual(result['state'], '/private/t0/registered/checkpoint')
        for key in ('armed', 'allow_orders', 'owner_authorized'):
            self.assertIs(result[key], False)
        self.assertIs(result['review_only'], True)
        self.assertFalse(report['can_arm'])
        self.assertNotIn('/private', json.dumps(report))
        self.assertTrue(all(ast.parse(text) for text in copies.values()))

    def test_real_core_helpers_share_canonical_module_and_repin_in_dependency_order(self):
        copies, cfg, _ = self.composed()
        mods = self.modules(copies)
        router, upstream, core = (mods[name] for name in ('router_recovery', 'upstream_recovery', 'working_fault_core'))
        self.assertIs(upstream.frozen, router)
        self.assertIs(upstream.Refused, router.Refused)
        self.assertEqual(upstream.PROBE_SHA, composition.sha(copies['router_recovery.py']))
        self.assertEqual(core.ROUTER_HELPER_SHA, cfg['router_helper_sha256'])
        self.assertEqual(core.UPSTREAM_HELPER_SHA, composition.sha(copies['upstream_recovery.py']))
        self.assertEqual(core.T1_LINKS_SHA, composition.T0_PROFILE)
        self.assertEqual(mods['short_fault_helpers'].T1_LINKS_SHA, composition.T0_PROFILE)
        adapter = mods['short_fault_helpers'].install(router, upstream)
        self.assertIs(mods['short_fault_helpers'].install(router, upstream), adapter)
        self.assertTrue(mods['short_fault_core'].install(core).Controller._short_fault_corrected)

    def test_held_guard_refuses_real_core_and_helpers_before_argument_parsing_or_actions(self):
        copies, _, _ = self.composed()
        mods = self.modules(copies)
        with patch.object(sys, 'argv', ['offline']), \
             patch.object(subprocess, 'Popen', side_effect=AssertionError('native process prohibited')):
            for name in ('router_recovery', 'upstream_recovery', 'working_fault_core'):
                with self.subTest(module=name), self.assertRaisesRegex(RuntimeError, '^t0_composition_held_unarmed$'):
                    mods[name].main()
            with self.assertRaisesRegex(RuntimeError, '^t0_composition_held_unarmed$'):
                mods['short_session'].run(None, {}, None, {}, None)

    def test_load_cfg_and_class_run_guard_precede_filesystem_and_native_effects(self):
        copies, _, _ = self.composed()
        for name in ('runtime.py', 'guardian.py'):
            scope = {}
            exec(copies[name], scope)
            with self.assertRaisesRegex(RuntimeError, '^t0_composition_held_unarmed$'):
                if name == 'runtime.py':
                    scope['load_cfg'](None)
                else:
                    scope['Guardian']().run()

    def test_bootstrap_setup_and_product_start_refuse_before_process_or_secret_access(self):
        sources = self.sources()
        sources['bootstrap.py'] = ('def setup(work,cfg):\n    raise AssertionError("secret read")\n'
            'class Product:\n    def start(self):\n        raise AssertionError("native process")\n')
        copies, _, _ = composition.compose(sources, self.config(), {})
        namespace = {}
        exec(copies['bootstrap.py'], namespace)
        with patch.object(subprocess, 'Popen', side_effect=AssertionError('native prohibited')):
            with self.assertRaisesRegex(RuntimeError, '^t0_composition_held_unarmed$'):
                namespace['setup'](None, {})
            with self.assertRaisesRegex(RuntimeError, '^t0_composition_held_unarmed$'):
                namespace['Product']().start()

    def test_rate_counts_submillisecond_sixth_post_as_failure_without_slack(self):
        mods = self.modules(self.composed()[0])
        function = mods['short_session'].burst_rate_metrics
        times = [1, 200_000_001, 400_000_001, 600_000_001, 800_000_001, 1_000_000_000]
        metrics = function(times)
        self.assertEqual(metrics['max_posts_per_rolling_second'], 6)
        self.assertEqual(metrics['max_posts_per_strict_ns_second'], 6)
        self.assertEqual(metrics['minimum_six_post_span_ns'], 999_999_999)
        self.assertEqual(metrics['rate_window_slack_ns'], 0)
        metrics = function(times[:-1] + [1_000_000_001])
        self.assertEqual(metrics['max_posts_per_rolling_second'], 5)
        self.assertEqual(metrics['minimum_six_post_span_ns'], 1_000_000_000)

    def test_invalid_native_time_and_weakened_risk_or_target_refused(self):
        mods = self.modules(self.composed()[0])
        for stamps in ([True], [0], [1.0]):
            with self.subTest(stamps=stamps), self.assertRaisesRegex(RuntimeError, 'native_post_timestamps'):
                mods['short_session'].burst_rate_metrics(stamps)
        for key, value in (('quantity_limit', 10), ('rate_limit', 6), ('router_port', 42101),
                           ('target_venue', 'T1'), ('source_git_sha', '0' * 40)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                composition.compose(self.sources(), dict(self.config(), **{key: value}), {})

    def test_t0_profile_and_date_postcheck_replacements_are_real(self):
        copies, _, _ = self.composed()
        for text in copies.values():
            self.assertNotIn(composition.T1_PROFILE, text)
            self.assertNotIn('links_public.t1.ini', text)
        self.assertIn('links_public.t0.ini', copies['upstream_recovery.py'])
        self.assertIn("'2026-10-21'", copies['guardian.py'])
        self.assertIn('dt.time(15, 40)', copies['short_postcheck.py'])
        self.assertNotIn('dt.time(23, 40)', copies['short_postcheck.py'])

    def test_inconsistent_helper_pins_and_invalid_ast_refused(self):
        sources = self.sources()
        sources['upstream_recovery.py'] = sources['upstream_recovery.py'].replace(
            '4c5fdc191de16ecd6cc6a7fac8f7c643cb15cd894c906924acbc7ce6a67737e4', '0' * 64)
        with self.assertRaisesRegex(ValueError, 'inconsistent_initial_helper_pins'):
            composition.compose(sources, self.config(), {})
        sources = self.sources()
        sources['runtime.py'] += '\ninvalid ('
        with self.assertRaisesRegex(ValueError, 'invalid_python'):
            composition.compose(sources, self.config(), {})

    def test_wrong_independent_profile_pin_refuses_even_with_correct_config(self):
        for name in ('working_fault_core.py', 'short_fault_helpers.py'):
            sources = self.sources()
            sources[name] = sources[name].replace(composition.T1_PROFILE, '0' * 64)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'profile_pin_inconsistent'):
                composition.compose(sources, self.config(), {})

    def test_one_line_entry_and_docstring_only_entry_remain_valid_and_held(self):
        sources = self.sources()
        sources['tiny.py'] = 'def run(): return 2\ndef main():\n    """entry only"""\n'
        copies, _, _ = composition.compose(sources, self.config(), {})
        namespace = {}
        exec(copies['tiny.py'], namespace)
        for name in ('run', 'main'):
            with self.assertRaisesRegex(RuntimeError, 'held_unarmed'):
                namespace[name]()

    def test_original_literal_review_lock_is_held_and_dynamic_or_inline_false_refused(self):
        sources = self.sources()
        sources['runtime.py'] = sources['runtime.py'].replace('REVIEW_ONLY = True', 'REVIEW_ONLY = False')
        copies, _, _ = composition.compose(sources, self.config(), {})
        self.assertIn('REVIEW_ONLY = True', copies['runtime.py'])
        for assignment in ('REVIEW_ONLY = bool(0)', 'x=0; REVIEW_ONLY = False'):
            sources['runtime.py'] = self.sources()['runtime.py'].replace('REVIEW_ONLY = True', assignment)
            with self.subTest(assignment=assignment), self.assertRaises(ValueError):
                composition.compose(sources, self.config(), {})

    def test_completed_final_run_keeps_working_sigterm_and_adds_missing_upstream(self):
        sources = self.sources()
        # Preserve actual method objects: only add the missing invocation to
        # the reviewed final-plan source representation.
        sources['short_session.py'] += '''\nclass FinalDriver(ShortDriver):
    def final_working_SIGTERM(self):
        return 'existing native proof'
    def run(self):
        self.stage("rate_burst_thirty",self.burst)
        self.stage("one_passive_router_restart",lambda:self.fault("one_passive_router_restart","router"))
        self.stage("planned_restart_two_working",self.restart_case)
        self.stage("working_order_SIGTERM",self.final_working_SIGTERM)
'''
        copies, _, _ = composition.compose(sources, self.config(), {})
        text = copies['short_session.py']
        self.assertEqual(text.count('self.stage("one_passive_upstream_socket_loss"'), 1)
        self.assertEqual(text.count('self.stage("working_order_SIGTERM"'), 1)
        self.assertEqual(text.count('def final_working_SIGTERM('), 1)


if __name__ == '__main__':
    unittest.main()
