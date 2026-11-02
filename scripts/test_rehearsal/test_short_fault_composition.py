"""Real frozen-core/helper composition; only external effects use offline fakes."""
from pathlib import Path
import copy
import contextlib
import hashlib
import importlib.machinery
import importlib.util
import json
import os
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

import short_fault_core
import short_fault_helpers
from partial_test.tests import fixtures

FIXTURES = Path(__file__).with_name('fixtures')
PINS = {
    'working_fault_core': '9abffaf38040efece9e622e100552e6e6b424f6a19662301f3b92bbf950890e7',
    'router_recovery': '4c5fdc191de16ecd6cc6a7fac8f7c643cb15cd894c906924acbc7ce6a67737e4',
    'upstream_recovery': 'eaf3a89d337f9f0c29bf66b0632ff45946b30030e39ec54d30a694da0ddd1712',
}


def frozen(name, logical=None):
    path = FIXTURES / (name + '.py.txt')
    assert hashlib.sha256(path.read_bytes()).hexdigest() == PINS[name]
    logical = logical or name
    loader = importlib.machinery.SourceFileLoader(logical, str(path))
    spec = importlib.util.spec_from_loader(logical, loader)
    module = importlib.util.module_from_spec(spec)
    sys.modules[logical] = module
    loader.exec_module(module)
    return module


class Clock:
    def __init__(self):
        self.now = 1000.

    def sleep(self, duration):
        self.now += max(.01, duration)


class Composition(unittest.TestCase):
    def setUp(self):
        self.old = {key: sys.modules.get(key) for key in PINS}
        self.addCleanup(self.modules)
        self.r = frozen('router_recovery')
        self.u = frozen('upstream_recovery')
        self.core = short_fault_core.install(frozen('working_fault_core'))
        self.helpers = short_fault_helpers.install(self.r, self.u)
        self.temp = tempfile.TemporaryDirectory(prefix='short-fault-real-composition-')
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.clock = Clock()
        self.patches = [patch.object(time_module, key, value) for time_module in {short_fault_core.time}
                        for key, value in [('time', lambda:self.clock.now),
                                           ('monotonic', lambda:self.clock.now),
                                           ('sleep', self.clock.sleep)]]
        for item in self.patches:
            item.start()
            self.addCleanup(item.stop)

    def modules(self):
        for key, value in self.old.items():
            if value is None:
                sys.modules.pop(key, None)
            else:
                sys.modules[key] = value

    def event(self, name, **data):
        with (self.work / 'interaction.ndjson').open('a') as out:
            out.write(json.dumps(dict(event=name, data=data)) + '\n')

    def controller(self, dry=False):
        state = fixtures.status(quantity_limit=2)
        state['configuration']['risk']['max_open_orders'] = 5
        state['configuration']['risk']['kill_switch'] = True
        state['configuration']['risk']['instruments'][0]['max_position'] = 2
        row = dict(instance_owned=True, operator_action_required=False, state='Working',
                   client_order_id='passive', order_id=1, ext_id=2, isin_id=fixtures.ISIN,
                   quantity=1, executed=0, remaining=1, price='0.000100')
        state['orders'] = [] if dry else [row]
        (self.work / 'interaction.ndjson').touch()
        words = ['MOEXJ2', '1', '2', '3', '100', '200', '4']
        checksum = 14695981039346656037
        for byte in ' '.join(words).encode():
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
        checkpoint = self.work / 'checkpoint'
        checkpoint.write_text(' '.join(words + [str(checksum)]))
        c = self.core.Controller(self.work, dict(source_git_sha='a'*40,
            entry_deadline_utc=1500., cleanup_deadline_utc=2000.))
        c.dry_run_no_order = dry
        c.product = types.SimpleNamespace(work=self.work, child=types.SimpleNamespace(pid=888,poll=lambda:None),
                                          status=lambda:copy.deepcopy(state))
        c.product_identity = dict(pid=888, executable='/not-a-real-product')
        c.environment = dict(state=checkpoint)
        c.passive_cid = 'passive'
        c.passive_target = dict(isin_id=fixtures.ISIN, quote_cap_scaled=10_000_000)
        c.sequence = types.SimpleNamespace(_price=lambda value:str(value), _units=lambda value:100)
        c.ops = types.SimpleNamespace(REQUIRED=set(fixtures.STREAMS), flat=lambda value:
            not value['orders'] and value['queued'] == 0 and not value['positions'])
        c.touch = lambda:None
        c.entry_gate = lambda:copy.deepcopy(state)
        c.wait_ready = lambda *args, **kwargs:copy.deepcopy(state)
        c.wait_fault_worker = lambda:None
        def raw(line):
            self.event('operator_input', line=line, channel='command_socket')
            self.event('local_refusal', line=line, channel='command_socket', error='kill switch enabled')
        c.raw_refusal_request = raw
        return c, state

    def test_original_fixture_hashes_and_actual_class_composed(self):
        self.assertTrue(self.core.Controller._short_fault_corrected)
        self.assertEqual(self.core.Controller.__mro__[1].__name__, 'Controller')
        self.assertIs(self.u.Refused, self.r.Refused)
        self.assertIs(short_fault_helpers.install(self.r, self.u), self.helpers)

    def test_duplicate_router_module_is_rejected_before_fault(self):
        duplicate = frozen('router_recovery', 'duplicate_router_for_test')
        try:
            with self.assertRaisesRegex(duplicate.Refused, 'canonical_router_helper_identity'):
                short_fault_helpers.install(duplicate, self.u)
        finally:
            sys.modules.pop('duplicate_router_for_test', None)

    def test_reply_listener_only_loss_is_native_outage(self):
        _, state = self.controller()
        self.assertFalse(short_fault_core.native_outage(state))
        next(row for row in state['transport']['listeners'] if row['stream'] == 'p2mqreply')['native']['state'] = 0
        self.assertTrue(short_fault_core.native_outage(state))

    def test_observed_outage_snapshot_is_used_without_second_status(self):
        c, state = self.controller()
        state['transport']['publisher']['state'] = 0
        folder = self.work / 'case'
        folder.mkdir()
        (folder / 'fault.json').write_text(json.dumps(dict(rollback_armed=True, upstream_fault=True,
                                                          interrupted_socket_count=4)))
        c.passive_price = 100
        c.product.status = lambda: self.fail('second status read lost observed outage')
        proof = c.bounded_outage_probe('case', folder, state)
        self.assertTrue(proof['local_refusal'])
        self.assertEqual(proof['native_posts'], 0)
        with self.assertRaisesRegex(self.core.Refused, 'must_not_be_retried'):
            c.bounded_outage_probe('case', folder, state)

    def test_local_refusal_may_flush_after_two_seconds_without_another_command(self):
        c, state = self.controller()
        state['transport']['publisher']['state'] = 0
        folder = self.work / 'case'
        folder.mkdir()
        (folder / 'fault.json').write_text(json.dumps(dict(rollback_armed=True, signal_sent=True)))
        c.passive_price = 100
        submitted = []
        def raw(line):
            submitted.append(line)
            self.event('operator_input', line=line, channel='command_socket')
        c.raw_refusal_request = raw
        original_sleep = self.clock.sleep
        def sleep(duration):
            original_sleep(duration)
            if self.clock.now >= 1002 and submitted:
                self.event('local_refusal', line=submitted.pop(), channel='command_socket', error='kill switch enabled')
        with patch.object(short_fault_core.time, 'sleep', side_effect=sleep):
            self.assertTrue(c.bounded_outage_probe('case', folder, state)['local_refusal'])
        self.assertGreaterEqual(self.clock.now, 1002)

    def raw_controller(self):
        c,state=self.controller()
        del c.raw_refusal_request
        c.binary=self.work/'native-binary'
        c.binary.write_bytes(b'protected offline executable')
        c.cfg['binary_sha256']=self.core.sha(c.binary)
        c.environment['env']={}
        c.ops.lock=lambda path:contextlib.nullcontext()
        c.ops.process_identity=lambda pid:c.product_identity if pid==888 else dict(pid=pid)
        c.ops.checked_signal=lambda *args:True
        c.product.guard=types.SimpleNamespace(prove=lambda:True)
        return c,state

    def test_actual_raw_probe_uses_ten_second_cli_deadline_and_one_attempt(self):
        c,_=self.raw_controller()
        child=types.SimpleNamespace(pid=999,returncode=1,
            communicate=Mock(return_value=(b'{"ok":false,"error":"kill switch enabled"}',b'')))
        with patch.object(short_fault_core.subprocess,'Popen',return_value=child) as spawned:
            c.raw_refusal_request('place unique 101 buy 1 1 day')
        self.assertEqual(spawned.call_count,1)
        child.communicate.assert_called_once_with(timeout=10)
        self.assertEqual(c.last_outage_probe_cli['timeout_seconds'],10)
        self.assertFalse(c.last_outage_probe_cli['timed_out'])

    def test_probe_cli_timeout_resolves_only_with_exact_completed_native_refusal(self):
        c,_=self.raw_controller()
        line='place unique 101 buy 1 1 day'
        deadlines=[]
        signals=[]
        def communicate(timeout):
            deadlines.append(timeout)
            if timeout==10:
                raise short_fault_core.subprocess.TimeoutExpired('offline cli',timeout)
            self.event('operator_input',line=line,channel='command_socket')
            self.event('local_refusal',line=line,channel='command_socket',error='kill switch enabled')
            return b'',b''
        child=types.SimpleNamespace(pid=999,returncode=-15,communicate=communicate)
        c.ops.checked_signal=lambda owner,sig:signals.append((owner,sig))
        with patch.object(short_fault_core.subprocess,'Popen',return_value=child) as spawned:
            c.raw_refusal_request(line)
        self.assertEqual(deadlines,[10,2])
        self.assertEqual(spawned.call_count,1)
        self.assertEqual(signals,[(dict(pid=999),self.r.signal.SIGTERM)])
        self.assertEqual(c.last_outage_probe_cli['outcome'],'NOT_SENT_NATIVE_REFUSAL_COMPLETED')
        self.assertTrue(c.last_outage_probe_cli['native_completion']['identifier_highwaters_unchanged'])

    def test_probe_timeout_absence_of_posts_and_allocations_is_still_uncertain(self):
        c,_=self.raw_controller()
        def communicate(timeout):
            if timeout==10:
                raise short_fault_core.subprocess.TimeoutExpired('offline cli',timeout)
            return b'',b''
        child=types.SimpleNamespace(pid=999,returncode=-15,communicate=communicate)
        with patch.object(short_fault_core.subprocess,'Popen',return_value=child) as spawned, \
             self.assertRaisesRegex(self.core.Refused,'outage_probe_uncertain_never_retried'):
            c.raw_refusal_request('place unique 101 buy 1 1 day')
        self.assertEqual(spawned.call_count,1)
        self.assertGreaterEqual(self.clock.now,1005)

    def test_probe_timeout_contradictory_native_proof_cannot_be_downgraded_to_not_sent(self):
        for contradiction in ('missing_input','posted','id_advance','guard_lost'):
            with self.subTest(contradiction=contradiction):
                c,_=self.raw_controller()
                (self.work/'interaction.ndjson').write_text('')
                line='place unique 101 buy 1 1 day'
                if contradiction=='guard_lost':
                    calls=[]
                    def guard():
                        calls.append(1)
                        if len(calls)>1:raise self.core.Refused('guard disabled')
                    c.product.guard.prove=guard
                def communicate(timeout):
                    if timeout==10:
                        raise short_fault_core.subprocess.TimeoutExpired('offline cli',timeout)
                    if contradiction!='missing_input':
                        self.event('operator_input',line=line,channel='command_socket')
                    self.event('local_refusal',line=line,channel='command_socket',error='kill switch enabled')
                    if contradiction=='posted':self.event('command_result',user_id=44)
                    return b'',b''
                child=types.SimpleNamespace(pid=999,returncode=-15,communicate=communicate)
                values=[(100,200),(101,200)] if contradiction=='id_advance' else [(100,200),(100,200)]
                with patch.object(short_fault_core.subprocess,'Popen',return_value=child) as spawned, \
                     patch.object(self.core,'checkpoint_highwaters',side_effect=values), \
                     self.assertRaisesRegex(self.core.Refused,'outage_probe_uncertain_'):
                    c.raw_refusal_request(line)
                self.assertEqual(spawned.call_count,1)

    def test_worker_exit_keeps_polling_for_later_native_outage(self):
        c, state = self.controller()
        c.product.status = lambda: copy.deepcopy(state if self.clock.now < 1000.10 else
            dict(state, transport=dict(state['transport'], publisher=dict(state=0))))
        def fork():
            folder = self.work / 'case'
            (folder / 'fault.json').write_text(json.dumps(dict(rollback_armed=True, upstream_fault=True,
                                                              interrupted_socket_count=4)))
            (folder / 'helper-result-private.json').write_text(json.dumps(dict(result={}, witness_offset=0)))
            self.event('link_lost', connection_state=3, publisher_state=0, reply_state=0)
            self.event('listener_open', stream='p2mqreply')
            self.event('listener_online', stream='p2mqreply')
            self.event('transaction_commit', stream='FORTS_TRADE_REPL')
            return 123
        with patch.object(short_fault_core.os, 'fork', side_effect=fork), \
             patch.object(short_fault_core.os, 'waitpid', return_value=(123, 0)):
            c.working_fault('case', 'upstream')
        self.assertEqual(c.public['cases'][0]['status'], 'PASS')
        self.assertGreaterEqual(self.clock.now, 1000.10)
        self.assertEqual(len(c.probe_attempts), 1)

    def test_fault_still_observes_outage_after_old_sixty_five_second_limit(self):
        c, state = self.controller()
        c.product.status = lambda:copy.deepcopy(state if self.clock.now < 1090 else
            dict(state,transport=dict(state['transport'],publisher=dict(state=0))))
        def fork():
            folder = self.work/'case'
            (folder/'fault.json').write_text(json.dumps(dict(rollback_armed=True,upstream_fault=True,
                                                            interrupted_socket_count=4)))
            (folder/'helper-result-private.json').write_text(json.dumps(dict(result={},witness_offset=0)))
            self.event('link_lost',connection_state=3,publisher_state=0,reply_state=0)
            self.event('listener_open',stream='p2mqreply')
            self.event('listener_online',stream='p2mqreply')
            self.event('transaction_commit',stream='FORTS_TRADE_REPL')
            return 123
        def wait(*args):
            return (123,0) if self.clock.now>=1100 else (0,0)
        with patch.object(short_fault_core.os,'fork',side_effect=fork), \
             patch.object(short_fault_core.os,'waitpid',side_effect=wait):
            c.working_fault('case','upstream')
        self.assertEqual(c.public['cases'][0]['status'],'PASS')
        self.assertGreaterEqual(self.clock.now,1100)

    def test_fault_key_check_refusal_must_clear_before_full_reconstruction_deadline(self):
        c, state = self.controller()
        del c.wait_ready  # Exercise the actual composed controller method.
        c.targets=[dict(isin_id=fixtures.ISIN,quote_cap_scaled=10_000_000)]
        c.initial_highwaters=(100,200)
        c.budget=lambda:9000
        c.inspect=lambda:None
        c.product.status=lambda:dict(state,cgate_key_check_failed=self.clock.now<1000.3)
        c.ops.ready=lambda value,targets,entry=False:not value['cgate_key_check_failed']
        self.assertFalse(c.wait_ready(timeout=2)['cgate_key_check_failed'])
        self.assertGreaterEqual(self.clock.now,1000.3)
        c.product.status=lambda:dict(state,cgate_key_check_failed=True)
        with self.assertRaisesRegex(self.core.Refused,'fault_readiness_or_flatness_deadline'):
            c.wait_ready(timeout=.2)

    def test_no_order_mode_skips_only_order_checks_and_still_requires_outage(self):
        c, state = self.controller(dry=True)
        self.assertIsNone(c.one_working(state))
        c.preserve_order(state, state)
        changed = copy.deepcopy(state)
        changed['orders'] = [dict(state='Working')]
        with self.assertRaisesRegex(self.core.Refused, 'complete_native_flatness'):
            c.one_working(changed)
        folder = self.work / 'case'
        folder.mkdir()
        (folder / 'fault.json').write_text(json.dumps(dict(rollback_armed=True, signal_sent=True)))
        c.passive_price = 1
        with self.assertRaisesRegex(self.core.Refused, 'native_outage_not_observed'):
            c.bounded_outage_probe('case', folder, state)

    def test_four_whitelisted_sockets_allow_address_rotation_and_shared_peer(self):
        white = types.SimpleNamespace(peers={('old', 1), ('new', 1)})
        rows = [types.SimpleNamespace(inode=str(i), peer=('new', 1)) for i in range(4)]
        self.assertTrue(self.helpers.restored(rows, white))
        self.assertFalse(self.helpers.restored(rows[:3], white))
        self.assertFalse(self.helpers.restored(rows + rows[:1], white))
        rows[-1].peer = ('unapproved', 1)
        self.assertFalse(self.helpers.restored(rows, white))

    def test_router_rollback_waits_past_five_seconds_for_exact_owned_exit(self):
        launch = types.SimpleNamespace(pid=10, start='same')
        probe = types.SimpleNamespace(executable=Path('/pinned/router'),
            owned_routers=lambda: [(10, Path('/pinned/router'))] if self.clock.now < 1010 else [])
        self.helpers.launch_context = lambda *args:'same'
        with patch.object(short_fault_helpers.os, 'kill', side_effect=AssertionError('unnecessary signal')):
            self.helpers.wait_owned_exit(probe, launch, 20)
        self.assertGreaterEqual(self.clock.now, 1010)

    def test_second_term_only_for_same_owned_identity_and_no_blind_relaunch(self):
        launch = types.SimpleNamespace(pid=10, start='same')
        signals = []
        probe = types.SimpleNamespace(executable=Path('/pinned/router'),
            owned_routers=lambda: [(10, Path('/pinned/router'))], clients=lambda *a, **k:1)
        self.helpers.launch_context = lambda *args:'same'
        with patch.object(short_fault_helpers.os, 'kill', side_effect=lambda *args:signals.append(args)):
            with self.assertRaisesRegex(self.r.Refused, 'did_not_exit_after_bounded_sigterm'):
                self.helpers.wait_owned_exit(probe, launch, 20, timeout=.1)
        self.assertEqual(signals, [(10, self.r.signal.SIGTERM)])
        self.helpers.launch_context = lambda *args:'changed'
        with patch.object(short_fault_helpers.os, 'kill', side_effect=AssertionError('unsafe signal')):
            with self.assertRaisesRegex(self.r.Refused, 'original_identity_changed'):
                self.helpers.wait_owned_exit(probe, launch, 20, timeout=.1)

    def test_second_term_does_not_depend_on_disappearing_listener_or_client(self):
        launch=types.SimpleNamespace(pid=10,start='same')
        active=[10]
        signals=[]
        probe=types.SimpleNamespace(executable=Path('/pinned/router'),
            owned_routers=lambda:[(10,Path('/pinned/router'))] if active else [],
            clients=lambda *a,**k:(_ for _ in ()).throw(self.r.Refused('router_listener_not_unique_owned')))
        self.helpers.launch_context=lambda *args:'same'
        def signal_owned(pid,sig):
            signals.append((pid,sig))
            active.clear()
        with patch.object(short_fault_helpers.os,'kill',side_effect=signal_owned):
            self.helpers.wait_owned_exit(probe,launch,20,timeout=.1)
        self.assertEqual(signals,[(10,self.r.signal.SIGTERM)])

    def test_descheduled_worker_refuses_signal_after_rollback_already_exited(self):
        launch=types.SimpleNamespace(pid=10,start='same')
        cfg=dict(environment='test',user_authorized=True,candidate_green=True,
            router_restart_authorized=True,router_restart_owner_pid=20,authorized_until_utc=1500)
        probe=types.SimpleNamespace(inspect=lambda *a,**k:(launch,dict(client_count=1)))
        def arm(*args):
            self.clock.sleep(self.r.ROLLBACK_DELAY+1)
            self.r.save(self.work/'rollback.json',dict(status='original_router_untouched',restored=True))
            return 42
        with patch.object(short_fault_helpers.os,'waitpid',return_value=(42,0)), \
             self.assertRaisesRegex(self.r.Refused,'rollback_not_alive_before_signal'):
            self.r.restart(probe,cfg,20,self.work,arm=arm,
                           send_signal=lambda *args:self.fail('router stopped without live rollback'))
        self.assertFalse((self.work/'router-stop-intent.json').exists())
        self.assertFalse((self.work/'fault.json').exists())

    def test_rollback_completion_receipt_prevents_signal_even_before_child_exit(self):
        launch=types.SimpleNamespace(pid=10,start='same')
        cfg=dict(environment='test',user_authorized=True,candidate_green=True,
            router_restart_authorized=True,router_restart_owner_pid=20,authorized_until_utc=1500)
        probe=types.SimpleNamespace(inspect=lambda *a,**k:(launch,dict(client_count=1)))
        def arm(*args):
            self.r.save(self.work/'rollback.json',dict(status='original_router_untouched',restored=True))
            return 42
        with patch.object(short_fault_helpers.os,'waitpid',return_value=(0,0)), \
             self.assertRaisesRegex(self.r.Refused,'rollback_not_alive_before_signal'):
            self.r.restart(probe,cfg,20,self.work,arm=arm,
                           send_signal=lambda *args:self.fail('router stopped after rollback completion'))
        self.assertFalse((self.work/'router-stop-intent.json').exists())

    def test_restart_holds_arbiter_until_stop_intent_and_signal_are_committed(self):
        launch=types.SimpleNamespace(pid=10,start='same')
        cfg=dict(environment='test',user_authorized=True,candidate_green=True,
            router_restart_authorized=True,router_restart_owner_pid=20,authorized_until_utc=1500)
        probe=types.SimpleNamespace(inspect=lambda *a,**k:(launch,dict(client_count=1)))
        calls=[]
        def send(pid,sig):
            calls.append((pid,sig))
            self.assertEqual(json.loads((self.work/'router-stop-intent.json').read_text()),dict(pid=10,start='same'))
            import fcntl
            with (self.work/'launch.lock').open('a') as concurrent:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(concurrent,fcntl.LOCK_EX|fcntl.LOCK_NB)
            self.r.save(self.work/'rollback.json',dict(restored=True))
        with patch.object(short_fault_helpers.os,'waitpid',side_effect=[(0,0),(42,0)]):
            result=self.r.restart(probe,cfg,20,self.work,arm=lambda *args:42,send_signal=send)
        self.assertEqual(calls,[(10,self.r.signal.SIGTERM)])
        self.assertEqual(result['status'],'router_restored')

    def test_restoration_arbiter_is_held_through_health_check(self):
        launch = types.SimpleNamespace(pid=10, start='same')
        probe = types.SimpleNamespace(executable=Path('/pinned/router'),
            owned_routers=lambda: [(10, Path('/pinned/router'))], clients=lambda *a, **k:1)
        self.helpers.launch_context = lambda *args:'same'
        white = types.SimpleNamespace(peers={('approved', 1)})
        def upstreams(*args):
            import fcntl
            with (self.work / 'launch.lock').open('a') as other:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return [types.SimpleNamespace(inode=str(i), peer=('approved', 1)) for i in range(4)]
        with patch.object(self.u, 'upstreams', side_effect=upstreams), \
             patch.object(short_fault_helpers.os, 'kill', side_effect=AssertionError('healthy router signalled')):
            self.assertEqual(self.helpers.restore_if_needed(probe, launch, 20, self.work, white, {('old',1)}), 0)

    def test_watchdog_never_accepts_old_healthy_router_with_pending_term_intent(self):
        launch=types.SimpleNamespace(pid=10,start='same')
        white=types.SimpleNamespace(peers={('approved',1)})
        active=[10]
        probe=types.SimpleNamespace(executable=Path('/pinned/router'),
            owned_routers=lambda:[(active[0],Path('/pinned/router'))] if active else [],
            clients=lambda *a,**k:1)
        (self.work/'router-stop-intent.json').write_text(json.dumps(dict(pid=10,start='same')))
        self.helpers.launch_context=lambda *args:'same'
        def wait_exit(*args):
            self.clock.sleep(8)
            active.clear()
        def launch_new(*args):
            active.append(11)
            return types.SimpleNamespace(pid=11)
        self.helpers.wait_owned_exit=wait_exit
        rows=[types.SimpleNamespace(inode=str(i),peer=('approved',1)) for i in range(4)]
        with patch.object(self.u,'upstreams',return_value=rows), \
             patch.object(self.u,'launch_router',side_effect=launch_new) as launcher:
            self.assertEqual(self.helpers.restore_if_needed(probe,launch,20,self.work,white,set()),0)
        self.assertEqual(active,[11])
        self.assertEqual(launcher.call_count,1)
        self.assertGreaterEqual(self.clock.now,1008)

    def test_canonical_refused_during_rebind_is_caught_and_retried(self):
        launch = types.SimpleNamespace(pid=10, start='same')
        calls = []
        def clients(*args, **kwargs):
            calls.append(1)
            if len(calls) == 1:
                raise self.u.Refused('router_listener_not_unique_owned')
            return 1
        probe = types.SimpleNamespace(executable=Path('/pinned/router'), owned_routers=lambda:[], clients=clients)
        self.helpers.launch_context = lambda *args:'same'
        with patch.object(self.u, 'launch_router', return_value=types.SimpleNamespace(pid=11,poll=lambda:None)):
            self.assertEqual(self.r.rollback(probe, launch, 20, self.work), 0)
        self.assertEqual(len(calls), 2)

    def test_armed_rollback_does_not_create_fault_if_pre_signal_check_refused(self):
        launch = types.SimpleNamespace(pid=10, start='same')
        white = types.SimpleNamespace(peers={('approved',1)})
        probe = types.SimpleNamespace(executable=Path('/pinned/router'),
            owned_routers=lambda:[(10,Path('/pinned/router'))], clients=lambda *a,**k:1)
        self.helpers.launch_context = lambda *args:'same'
        rows = [types.SimpleNamespace(inode=str(i),peer=('approved',1)) for i in range(4)]
        with patch.object(self.u,'pristine_whitelist',return_value=white), \
             patch.object(self.u,'upstreams',return_value=rows), \
             patch.object(short_fault_helpers.os,'kill',side_effect=AssertionError('new fault in rollback')), \
             patch.object(self.u,'launch_router',side_effect=AssertionError('duplicate router')):
            self.assertEqual(self.r.rollback(probe,launch,20,self.work),0)
        self.assertEqual(json.loads((self.work/'rollback.json').read_text())['status'],'original_router_untouched')

    def test_whitelist_dns_cached_but_exact_configuration_is_rehashed(self):
        launch = types.SimpleNamespace(argv=(b'router',), env={b'key':b'value'}, cwd=self.work)
        probe = types.SimpleNamespace(executable=self.work/'router')
        white = types.SimpleNamespace(peers={('approved',1)})
        calls = []
        def original(*args, **kwargs):
            calls.append(1)
            if len(calls) == 1:
                raise self.u.socket.gaierror('temporary DNS failure')
            return white
        self.helpers.original_whitelist = original
        self.helpers.files = lambda *args:(('config', 'unchanged'),)
        self.assertIs(self.u.pristine_whitelist(probe, launch, 'pin'), white)
        self.assertIs(self.u.pristine_whitelist(probe, launch, 'pin'), white)
        self.assertEqual(len(calls), 2)
        self.helpers.files = lambda *args:(('config', 'changed'),)
        with self.assertRaisesRegex(self.r.Refused, 'cached_t1_configuration_changed'):
            self.u.pristine_whitelist(probe, launch, 'pin')

    def test_same_owned_reconnect_pair_is_pending_but_foreign_pair_refuses(self):
        router = self.work/'router'
        router.write_text('pinned fixture')
        proc = self.work/'proc'
        (proc/'20').mkdir(parents=True)
        (proc/'20'/'exe').symlink_to(router)
        # Encoded rows are abstract here; the real frozen endpoint function is
        # retained by production. Only its byte decoding boundary is injected.
        rows = [('local', 'peer1','01','s1'),('peer1','local','01','p1'),
                ('local','peer2','01','s2'),('peer2','local','01','p2')]
        sockets = {10:{'s1','s2'}, 20:{'p1','p2'}}
        probe = types.SimpleNamespace(proc=proc, executable=router, identity=lambda *args:'same',
            sockets=lambda pid:sockets[pid], tcp=lambda:rows)
        self.helpers.original_local = lambda *args: (_ for _ in ()).throw(self.r.Refused('other_router_clients'))
        with patch.object(self.u, 'endpoint', side_effect=lambda text:('127.0.0.1',4101 if text=='local' else 5000)):
            self.assertIsNone(self.helpers.local_connection(probe,10,20,allow_reconnect=True))
            sockets[20] = {'p1'}
            with self.assertRaisesRegex(self.r.Refused,'other_router_clients'):
                self.helpers.local_connection(probe,10,20,allow_reconnect=True)

    def test_actual_upstream_helper_composition_setup_cut_and_alternate_peer_recovery(self):
        binary=self.work/'product'
        binary.write_bytes(b'offline native boundary')
        router_exe=self.work/'router'
        router_exe.write_bytes(b'offline router boundary')
        original=types.SimpleNamespace(pid=10,start='same',argv=[b'router'],env={},cwd=self.work)
        active=[10]
        cut=[]
        signals=[]
        acknowledgements=[]
        old=[types.SimpleNamespace(fd=i,inode='old'+str(i),peer=('old',1)) for i in range(4)]
        new=[types.SimpleNamespace(fd=i+4,inode='new'+str(i),peer=('new',1)) for i in range(4)]
        white=types.SimpleNamespace(peers={('old',1),('new',1)},file_sha256=short_fault_helpers.T1_LINKS_SHA)
        cfg=dict(environment='test',user_authorized=True,candidate_green=True,
            router_restart_authorized=True,router_restart_owner_pid=20,
            upstream_fault_authorized=True,upstream_owner_pid=20,
            upstream_harness_sha256=PINS['upstream_recovery'],
            test_upstream_file_sha256=short_fault_helpers.T1_LINKS_SHA,
            authorized_until_utc=1500,binary=str(binary),binary_sha256=self.r.digest(binary))
        probe=types.SimpleNamespace(executable=router_exe,
            identity=lambda *args:'same',owned_routers=lambda:[(active[0],router_exe)] if active else [],
            inspect=lambda *a,**k:(types.SimpleNamespace(**dict(original.__dict__,pid=active[0])),dict(client_count=1)),
            clients=lambda *a,**k:1,
            tcp=lambda:[('local','peer','01',row.inode) for row in (new if len(cut)==4 else old)])
        class Duplicate:
            def __init__(self,fd):self.fd=fd
            def shutdown(duplicate,how):cut.append(duplicate.fd)
            def close(duplicate):pass
        def send(pid,sig):
            signals.append((pid,sig))
            self.assertEqual(active,[10])
            active.clear()
        def launch(*args):
            self.assertFalse(active)
            active.append(11)
            return types.SimpleNamespace(pid=11,poll=lambda:None)
        def acknowledge():
            acknowledgements.append(True)
            self.assertEqual(active,[11])
            self.assertFalse(cut)
        self.helpers.launch_context=lambda *args:'same'
        read_fd,write_fd=os.pipe()
        try:
            with patch.object(self.u,'ensure_pidfd_api'), \
                 patch.object(self.u,'pristine_whitelist',return_value=white), \
                 patch.object(self.u,'upstreams',side_effect=lambda *a:new if len(cut)==4 else old), \
                 patch.object(self.u,'arm_watchdog',return_value=(42,write_fd)), \
                 patch.object(self.u,'child_identity',return_value='same'), \
                 patch.object(self.u,'launch_router',side_effect=launch), \
                 patch.object(self.u,'local_connection',return_value='loopback'), \
                 patch.object(self.u,'duplicate_verified',return_value=[Duplicate(i) for i in range(4)]), \
                 patch.object(self.u,'require_watchdog_live'), \
                 patch.object(short_fault_helpers.os,'waitpid',return_value=(42,0)), \
                 patch.object(short_fault_helpers.os,'kill',side_effect=send):
                result=self.u.interrupt(probe,cfg,20,self.work,999,acknowledge=acknowledge)
            self.assertEqual(signals,[(10,self.r.signal.SIGTERM)])
            self.assertEqual(acknowledgements,[True])
            self.assertEqual(cut,list(range(4)))
            self.assertEqual(result['stage'],'UPSTREAM_RESTORED')
            self.assertEqual(result['restored_socket_count'],4)
            self.assertTrue(result['router_process_preserved'])
            self.assertEqual(os.read(read_fd,1),b'D')
        finally:
            os.close(read_fd)


if __name__ == '__main__':
    unittest.main()
