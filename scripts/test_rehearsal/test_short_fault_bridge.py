"""Offline only: no product, fork, subprocess, network, signal or router fault."""
from pathlib import Path
import copy
import hashlib
import json
import os
import tempfile
import types
import unittest
from unittest.mock import patch


import short_fault_bridge as h


class Refused(RuntimeError):
    pass


def require(condition, code):
    if not condition:
        raise Refused(code)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def same_launch(original, current):
    require((original.argv, original.env, original.cwd) ==
            (current.argv, current.env, current.cwd), 'router_context_changed')


# The deployed, reviewed fault core is injected into the actual bridge. Tests
# mock that dependency instead of importing private source or native helpers.
old = types.SimpleNamespace(Refused=Refused, require=require, sha=sha)


class FakeProbe:
    def __init__(self, router, identity):
        self.executable, self.identity = router, identity
        self.expected_sha = old.sha(router)
        self.calls, self.failures = 0, []
        self.launch = types.SimpleNamespace(pid=identity['pid'], argv=('router',), env={'same': 'yes'}, cwd='/same')

    def owned_routers(self):
        return [(self.identity['pid'], str(self.executable))]

    def inspect(self, *args, **kwargs):
        self.calls += 1
        if self.failures:
            value = self.failures.pop(0)
            if value:
                raise value
        return self.launch, {'client_count': 1}


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='oct16-bridge-offline-')
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.q = self.home / 'moex/qualification'
        self.q.mkdir(parents=True)
        self.work = self.q / 'new/day'
        self.work.mkdir(parents=True)
        self.binary, self.routerbin = self.q / 'binary', self.q / 'P2MQRouter'
        self.binary.write_bytes(b'fixture product')
        self.routerbin.write_bytes(b'fixture router')
        self.productid = dict(pid=333, start_ticks=12, executable=str(self.binary))
        self.routerid = dict(pid=222, start_ticks=11, executable=str(self.routerbin))
        self.identities = {333: self.productid, 222: self.routerid}
        self.raw = FakeProbe(self.routerbin, self.routerid)
        self.meta = dict(identity=self.productid, binary=str(self.binary), binary_sha256=old.sha(self.binary))
        self.ops = types.SimpleNamespace(process_identity=lambda pid: copy.deepcopy(self.identities[pid]))
        self.probe = h.BoundedProbe(self.raw, self.ops, self.meta, old.require, old.sha)
        self.routerfile, self.upstreamfile = self.q / 'router.py', self.q / 'upstream.py'
        self.routerfile.write_text('fixture router helper')
        self.upstreamfile.write_text('fixture upstream helper')
        self.core = types.SimpleNamespace(Refused=Refused, require=require, sha=sha, same_launch=same_launch,
            PINNED_SOURCE='d'*40, authorized=lambda *a, **kw: None, T1_LINKS_SHA='e'*64)
        self.core.ROUTER_HELPER_SHA = old.sha(self.routerfile)
        self.core.UPSTREAM_HELPER_SHA = old.sha(self.upstreamfile)
        self.core.checkpoint_highwaters = lambda path: (100, 200)
        self.instances = []
        owner = self
        core = self.core
        class Controller:
            def __init__(self, work, cfg):
                self.work, self.cfg = work, cfg
                self.public, self.guardian, self.fault_worker = {'cases': []}, None, None
                owner.instances.append(self)
            def full_state(self, entry=False):
                state = self.product.status()
                require(self.ops.ready(state, self.targets, entry=entry), 'native_not_ready')
                return state
            def one_working(self, state):
                rows = [r for r in state['orders'] if r['state'] not in {'Filled','Cancelled','Rejected'}]
                require(len(rows) == 1 and rows[0]['instance_owned'] is True and
                        rows[0]['quantity'] == rows[0]['remaining'] == 1 and
                        rows[0]['executed'] == 0 and state['queued'] == 0 and
                        all(r['xpos'] == 0 for r in state['positions']), 'one_working_unproved')
            def fault_authorization(self):
                core.authorized(self.cfg)
                require(core.PINNED_SOURCE == self.cfg['source_git_sha'], 'core_source_not_adapted')
            def working_fault(self, name, kind):
                state = self.entry_gate()
                require(state['configuration']['risk']['kill_switch'] is True, 'core_guard_field_missing')
                self.fault_authorization()  # Original method must see the new bounded authorization.
                if getattr(owner, 'pending', False):
                    self.fault_worker = 12345
                    raise old.Refused('offline_helper_pending')
                if getattr(owner, 'failed_after_exit', False):
                    raise old.Refused('offline_helper_exited_but_restore_unknown')
                self.public['cases'].append(dict(case=name, status='PASS', working_order_preserved=True,
                    local_refusal=dict(local_refusal=True, native_posts=0,
                        identifier_highwaters_unchanged=True, probe_attempts=1),
                    native_witness=dict(observed_native_outage=True,
                        all_streams_current_native_ready=True, fresh_trade_commits=1)))
            def wait_fault_worker(self):
                if self.fault_worker:
                    raise old.Refused('offline_restore_pending')
        self.core.Controller = Controller
        self.cfg = dict(authorized_date=h.DATE, environment='test', owner_authorized=True,
            timer_paused=True, one_passive_order_authorized=True, skip_firewall=True,
            max_postday_seconds=21600, source_git_sha='a'*40,
            binary=str(self.binary), binary_sha256=old.sha(self.binary),
            router_helper_sha256=self.core.ROUTER_HELPER_SHA,
            upstream_helper_sha256=self.core.UPSTREAM_HELPER_SHA,
            scheduled_start_utc=h.epoch(9), earliest_entry_utc=h.epoch(10),
            product_start_latest_utc=h.epoch(10,10),
            admission_latest_utc=h.epoch(12), hard_entry_deadline_utc=h.epoch(14,30),
            flat_deadline_utc=h.epoch(14,40), guardian_trigger_utc=h.epoch(14,55),
            hard_cleanup_deadline_utc=h.epoch(15),
            fault_lock=str(self.q/'router-fault.lock'))
        self.targets = [dict(symbol='ALRS-12.26', isin_id=101, quote_cap_scaled=200),
                        dict(symbol='CNY-12.26', isin_id=102, quote_cap_scaled=200)]
        self.state = dict(queued=0, positions=[], orders=[dict(client_order_id='passive',
            instance_owned=True, state='Working', quantity=1, remaining=1, executed=0)],
            configuration=dict(allow_orders=True, sole_instance=True, login_configured=True, rate=5,
                risk=dict(max_quantity=2, max_open_orders=5, instruments=[dict(isin_id=t['isin_id'],
                    max_position=2, max_quote_notional_scaled=200) for t in self.targets])))
        self.product = types.SimpleNamespace(work=self.work, child=types.SimpleNamespace(pid=333),
                                             status=lambda:copy.deepcopy(self.state))
        self.journal = self.work/'interaction.ndjson'
        self.journal.write_text('')
        self.event('startup')
        self.event('operator_input', line='kill on', channel='command_socket')
        self.event('kill_switch', enabled=True)
        words = ['MOEXJ2','1','2','3','100','200','4']
        checksum = 14695981039346656037
        for byte in ' '.join(words).encode():
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
        (self.q/'identity').write_text(' '.join(words+[str(checksum)])+'\n')
        self.ops.ready = lambda state, targets, entry=False: True
        guard = types.SimpleNamespace(strict_upstreams=lambda *a: [types.SimpleNamespace(peer=i) for i in range(4)])
        self.rt = types.SimpleNamespace(HEAD='a'*40, BINARY=self.cfg['binary_sha256'],
            budget=lambda meta: 9000, _probe=lambda:(self.raw, guard, None),
            verify_owner=lambda meta: None, configuration_gate=lambda *args: None)
        self.s = types.SimpleNamespace(cfg=self.cfg, meta=self.meta, product=self.product,
            environment=dict(state=self.q/'identity'), targets=self.targets, ops=self.ops, rt=self.rt,
            root=self.work.parent, work=self.work, sequence=types.SimpleNamespace(),
            prices=types.SimpleNamespace(current_reference=lambda *args:100),
            tick=lambda:None, publish=lambda value:None)
        self.router = types.SimpleNamespace(__file__=str(self.routerfile))
        self.upstream = types.SimpleNamespace(__file__=str(self.upstreamfile), pristine_whitelist=lambda *a:'white')
        self.now = patch.object(h.time, 'time', return_value=h.epoch(10,5))
        self.now.start(); self.addCleanup(self.now.stop)
        self.homepatch = patch.object(Path, 'home', return_value=self.home)
        self.homepatch.start(); self.addCleanup(self.homepatch.stop)

    def event(self, name, **data):
        with self.journal.open('a') as stream:
            stream.write(json.dumps(dict(event=name, data=data))+'\n')

    def callback(self):
        return h.build_faultcase(self.s, self.core, self.router, self.upstream)

    def test_import_and_callback_construction_are_inert(self):
        with patch.object(os, 'fork', side_effect=AssertionError('unexpected fork')):
            self.callback()
        self.assertEqual(self.instances, [])
        self.assertEqual(self.raw.calls, 0)

    def dry_cfg(self):
        live = self.q/'live-identity'
        live.write_bytes(b'untouched live checkpoint')
        allocation = self.q/'dedicated-assignment.json'
        allocation.write_text(json.dumps(dict(ext_id_begin=50000,ext_id_end=59999)))
        allocation.chmod(0o600)
        now = h.epoch(18)-86400
        cfg=dict(self.cfg,entry_deadline_utc=now+1200,cleanup_deadline_utc=now+1800,
            zero_order_rehearsal_authorized=True,dry_run_authorized_date='2026-10-15',
            dry_run_until_utc=now+1200,dry_run_cleanup_until_utc=now+1800,
            dry_run_live_state=str(live),dry_run_state=str(self.q/'identity'),
            dry_run_live_checkpoint_sha256=sha(live),dry_run_assignment=str(allocation),
            dry_run_assignment_sha256=sha(allocation),dry_run_live_ext_id_begin=10000,
            dry_run_live_ext_id_end=49999)
        return cfg,now

    def test_tonight_zero_order_authorization_requires_separate_state_and_ids(self):
        cfg,now=self.dry_cfg()
        h.authorized(self.core,self.s,cfg,now=now,dry_run_no_order=True)
        for key,value in [('zero_order_rehearsal_authorized',False),
                          ('dry_run_authorized_date','2026-10-16'),
                          ('dry_run_until_utc',now+1201),
                          ('dry_run_state',cfg['dry_run_live_state']),
                          ('dry_run_live_ext_id_end',55000)]:
            with self.subTest(key=key),self.assertRaises(Refused):
                h.authorized(self.core,self.s,dict(cfg,**{key:value}),now=now,dry_run_no_order=True)

    def test_october_fifteen_never_authorizes_normal_working_order_fault(self):
        cfg,now=self.dry_cfg()
        with self.assertRaises(Refused):
            h.authorized(self.core,self.s,cfg,now=now,dry_run_no_order=False)
        with self.assertRaises(Refused):
            h.authorized(self.core,self.s,dict(cfg,authorized_date='2026-10-15'),now=now,
                         dry_run_no_order=False)

    def test_dry_rehearsal_rejects_hardlinked_live_checkpoint_and_changed_assignment(self):
        cfg,now=self.dry_cfg()
        isolated=Path(cfg['dry_run_state'])
        isolated.unlink()
        os.link(cfg['dry_run_live_state'],isolated)
        with self.assertRaisesRegex(Refused,'isolated_state_unproved'):
            h.authorized(self.core,self.s,cfg,now=now,dry_run_no_order=True)
        isolated.unlink();isolated.write_text('isolated')
        Path(cfg['dry_run_assignment']).write_text('{}')
        with self.assertRaisesRegex(Refused,'assignment_unproved'):
            h.authorized(self.core,self.s,cfg,now=now,dry_run_no_order=True)

    def test_only_fd_symlink_race_restarts_whole_inspection(self):
        self.raw.failures = [FileNotFoundError(2, 'gone', '/proc/222/fd/9'), None]
        with patch.object(h.time, 'sleep'):
            result = self.probe.inspect({'environment':'test'})
        self.assertEqual(result[1]['client_count'], 1)
        self.assertEqual(self.raw.calls, 2)

    def test_missing_process_or_other_file_does_not_retry(self):
        for path in ('/proc/222/stat', '/etc/private', '/proc/x/fd/4'):
            self.raw.failures = [FileNotFoundError(2, 'gone', path)]
            before = self.raw.calls
            with self.assertRaises(FileNotFoundError):
                self.probe.inspect({})
            self.assertEqual(self.raw.calls, before + 1)

    def test_product_identity_change_during_fd_race_refuses(self):
        def changed(*args, **kwargs):
            self.identities[333]['start_ticks'] += 1
            raise FileNotFoundError(2, 'gone', '/proc/222/fd/9')
        self.meta['identity'] = copy.deepcopy(self.productid)
        with patch.object(self.raw, 'inspect', side_effect=changed):
            with self.assertRaisesRegex(old.Refused, 'product_identity_changed'):
                self.probe.inspect({})

    def test_three_fd_races_fail_without_partial_inventory(self):
        self.raw.failures = [FileNotFoundError(2, 'gone', '/proc/222/fd/9') for _ in range(3)]
        with patch.object(h.time, 'sleep'), self.assertRaisesRegex(old.Refused, 'race_exhausted'):
            self.probe.inspect({})
        self.assertEqual(self.raw.calls, 3)

    def test_case_reuses_core_and_preserves_root_product_and_guardian(self):
        prior_auth, prior_pin = self.core.authorized, self.core.PINNED_SOURCE
        with patch.object(os, 'fork', side_effect=AssertionError('unexpected fork')):
            result = self.callback()(self.product,'router_case','router',None,'passive',self.targets[0])
        self.assertEqual(result['status'], 'PASS')
        self.assertTrue(result['working_order_preserved'])
        self.assertIn('not_transport_causality', result['local_refusal']['causal_scope'])
        self.assertIs(self.s.product, self.product)
        self.assertIsNone(self.instances[0].guardian)
        self.assertIs(self.core.authorized, prior_auth)
        self.assertEqual(self.core.PINNED_SOURCE, prior_pin)
        self.assertTrue(self.instances[0].fault_lock.closed)
        self.assertNotIn('kill_switch', self.state['configuration']['risk'])
        self.assertIs(self.s.fault_restore_required, False)
        self.assertEqual(result['kill_guard_provenance']['proof'],
                         'native_operator_kill_on_then_kill_switch_enabled')

    def test_wrong_day_pin_scope_deadlines_never_reach_core(self):
        for key, value in (('authorized_date','2026-10-17'),('source_git_sha','b'*40),
                           ('timer_paused',False),('max_postday_seconds',300),('skip_firewall',False),
                           ('hard_entry_deadline_utc',h.epoch(11,59))):
            original = self.cfg[key]
            self.cfg[key] = value
            with self.subTest(key=key), self.assertRaises(old.Refused):
                self.callback()(self.product,'case','router',None,'passive',self.targets[0])
            self.cfg[key] = original
        self.assertEqual(self.instances, [])

    def test_helper_bytes_change_refuses_before_fault(self):
        self.routerfile.write_text('changed')
        with self.assertRaisesRegex(old.Refused, 'helper_bytes_changed'):
            self.callback()(self.product,'case','router',None,'passive',self.targets[0])
        self.assertEqual(self.instances, [])

    def test_global_lock_symlink_and_foreign_path_refused(self):
        for path in (self.work/'wrong-name', self.home/'router-fault.lock'):
            self.cfg['fault_lock'] = str(path)
            with self.assertRaisesRegex(old.Refused, 'global_lock_path'):
                self.callback()(self.product,'case','router',None,'passive',self.targets[0])
        symlink = self.q/'router-fault.lock'
        symlink.symlink_to(self.binary)
        self.cfg['fault_lock'] = str(symlink)
        with self.assertRaisesRegex(old.Refused, 'global_lock_path'):
            self.callback()(self.product,'case','router',None,'passive',self.targets[0])


    def test_fork_worker_wait_uses_proxy_owned_process_never_parent_popen(self):
        owner = self
        class Product:
            def __init__(self):
                self.child = types.SimpleNamespace(poll=lambda:owner.fail('parent Popen polled in worker'))
            def status(self):
                return copy.deepcopy(owner.state)
            def wait_status(self, until):
                require(self.child.poll() is None, 'product_exited')
                return self.status()
            def wait_ready(self, until, **kwargs):
                require(self.child.poll() is None, 'product_exited')
                return self.wait_status(until)
        original = Product()
        proxy = h.GuardedStatusProxy(original, h.NativeKillGuard(self.journal, require, lambda:None))
        proxy.child = types.SimpleNamespace(poll=lambda:None)
        self.assertTrue(proxy.wait_ready(123)['configuration']['risk']['kill_switch'])
        self.assertIsNot(proxy.child, original.child)

    def test_guard_rejects_enabled_record_without_exact_current_operator_request(self):
        variants = [
            [('startup', {}), ('kill_switch', {'enabled': True})],
            [('operator_input', {'line':'kill on','channel':'stdin'}), ('kill_switch',{'enabled':True})],
            [('operator_input', {'line':'kill on','channel':'command_socket'}),
             ('kill_switch',{'enabled':True}), ('startup',{})],
            [('operator_input', {'line':'kill on','channel':'command_socket'}),
             ('kill_switch',{'enabled':True}),
             ('operator_input', {'line':'kill off','channel':'command_socket'})],
            [('operator_input', {'line':'kill on','channel':'command_socket'}),
             ('kill_switch',{'enabled':True}), ('kill_switch',{'enabled':False})],
        ]
        for rows in variants:
            with self.subTest(rows=rows):
                self.journal.write_text('')
                for name, data in rows:
                    self.event(name, **data)
                guard = h.NativeKillGuard(self.journal, require, lambda:None)
                with patch.object(h.time, 'monotonic', side_effect=[1,7]):
                    with self.assertRaisesRegex(Refused, 'native_kill_guard_unproved'):
                        guard.prove()

    def test_guard_waits_only_for_buffered_native_enabled_record(self):
        self.journal.write_text('')
        self.event('operator_input', line='kill on', channel='command_socket')
        guard = h.NativeKillGuard(self.journal, require, lambda:None)
        with patch.object(h.time, 'sleep', side_effect=lambda _:self.event('kill_switch',enabled=True)):
            self.assertEqual(guard.prove()['operator_input_offset'], 0)

    def test_guard_rejects_truncation_or_replaced_journal(self):
        for mode in ('truncate','replace'):
            with self.subTest(mode=mode):
                self.journal.write_text('')
                self.event('operator_input',line='kill on',channel='command_socket')
                self.event('kill_switch',enabled=True)
                guard = h.NativeKillGuard(self.journal, require, lambda:None)
                guard.prove()
                if mode == 'truncate':
                    self.journal.write_text('')
                else:
                    other = self.journal.with_suffix('.new')
                    other.write_bytes(self.journal.read_bytes())
                    other.replace(self.journal)
                with self.assertRaisesRegex(Refused,'guard_journal_changed'):
                    guard.prove()

    def test_pending_helper_never_returns_pass_or_unlocks_restoration(self):
        self.pending = True
        prior = self.core.authorized
        with self.assertRaisesRegex(old.Refused, 'restore_pending'):
            self.callback()(self.product,'case','router',None,'passive',self.targets[0])
        c = self.s.pending_fault_controller
        self.assertEqual(c.fault_worker, 12345)
        self.assertFalse(c.fault_lock.closed)
        self.assertIs(self.core.authorized, prior)
        self.assertIs(self.s.fault_restore_required, True)
        # Test fixture owns harmless descriptors; no helper was actually created.
        os.close(c.guard_control); c.fault_lock.close()

    def test_worker_exit_alone_does_not_clear_restoration_requirement(self):
        self.failed_after_exit = True
        with self.assertRaisesRegex(old.Refused,'restore_unknown'):
            self.callback()(self.product,'case','router',None,'passive',self.targets[0])
        self.assertIs(self.s.fault_restore_required,True)
        self.assertIsNone(self.instances[-1].fault_worker)


if __name__ == '__main__':
    unittest.main()
