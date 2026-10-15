"""Portable control-plane regressions; all external/runtime APIs are mocked."""
import copy
import datetime as dt
import importlib.util
import hashlib
import tempfile
import json
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch


class Refused(RuntimeError):
    pass


def require(value, code):
    if not value:
        raise Refused(code)


def adjacent(name, filename, dependency_name, dependency):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {dependency_name: dependency}):
        spec.loader.exec_module(module)
    return module


class ParentSupervisor:
    def start(self):
        return self.product
    def risk_gate(self, product):
        self.events.append("native_risk_gate")
        return product
    def fail(self, error):
        self.events.append("base_cleanup")
        return "cleanup_result"


super_base = types.ModuleType("base_supervisor")
super_base.MSK = dt.timezone(dt.timedelta(hours=3))
super_base.HEAD = "4ee96ce0cb41f0228fb74669f75d8be7a896c813"
super_base.BINARY = "3da6b0db4f36bbc53a9e173906315bc78cf55a7e2e7ee547bb7b9bdcedca27be"
super_base.require, super_base.Supervisor = require, ParentSupervisor
super_base.Refused = Refused
supervisor = adjacent("_short_supervisor_test", "short_supervisor.py", "base_supervisor", super_base)

runtime_base = types.ModuleType("frozen_runtime")
runtime_base.HEAD, runtime_base.BINARY = super_base.HEAD, super_base.BINARY
runtime_base.BOOT, runtime_base.CI_RUN_ID = "a" * 64, 38579249212
runtime_base.require = require
runtime_base.atomic = lambda *_: None
runtime = adjacent("_short_runtime_test", "short_runtime.py", "frozen_runtime", runtime_base)


def configuration():
    cfg = dict(authorized_date="2026-10-16", owner_authorized=True, timer_paused=True,
               no_watchdog_sigkill=True, source_git_sha=super_base.HEAD,
               binary_sha256=super_base.BINARY, coverage_scope="SHORT_INTERRUPTION_ONLY",
               closeout_mode="flat_then_observe_clearing", latency_gating=False,
               partial_fill_enabled=False, predecessor_clean_terminal_verified=True,
               environment="test", quantity_limit=2, position_limit=2, open_order_limit=5,
               rate_limit=5, review_git_sha="b" * 40, review_ci_run_id=123,
               ci_run_id=runtime.CI_RUN_ID)
    import short_scope
    cfg.update(short_scope.expected(cfg['authorized_date']))
    return cfg


class ScopeTests(unittest.TestCase):
    def test_only_exact_owner_scope_and_fixed_risk_pass(self):
        supervisor.validate(configuration())
        changes = [("authorized_date", "2026-10-20"), ("owner_authorized", False),
                   ("timer_paused", False), ("no_watchdog_sigkill", False),
                   ("source_git_sha", "c" * 40), ("binary_sha256", "d" * 64),
                   ("coverage_scope", "FULL_DAY"), ("closeout_mode", "open_session_sigterm"),
                   ("latency_gating", True), ("partial_fill_enabled", True),
                   ("predecessor_clean_terminal_verified", False), ("environment", "prod"),
                   ("quantity_limit", 10), ("position_limit", 10),
                   ("open_order_limit", 6), ("rate_limit", 6)]
        for key, value in changes:
            with self.subTest(key=key), self.assertRaises((Refused, ValueError)):
                supervisor.validate(dict(configuration(), **{key: value}))

    def test_every_sealed_start_entry_fault_and_cleanup_boundary_is_exact(self):
        for key in ("scheduled_start_utc", "product_start_latest_utc", "admission_latest_utc",
                    "earliest_entry_utc", "hard_entry_deadline_utc", "flat_deadline_utc",
                    "guardian_trigger_utc", "hard_cleanup_deadline_utc"):
            cfg = configuration(); cfg[key] += 1
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "sealed_window"):
                supervisor.validate(cfg)

    def test_two_start_headroom_checked_before_start_and_native_gate_stays_7000(self):
        runner = supervisor.Supervisor(); runner.environment = {'state': 'state'}
        runner.cfg = dict(configuration(), ext_id_range='1:100000', instance='owned')
        for budget in (6999, 7000, 7999, 8999):
            runner.rt = types.SimpleNamespace(budget=lambda _, n=budget: n)
            with self.subTest(budget=budget), self.assertRaisesRegex(Refused, 'headroom'):
                runner.prestart_headroom()
        runner.rt = types.SimpleNamespace(budget=lambda _: 9999)
        runner.prestart_headroom()

    def test_monday_scope_and_exact_unchanged_limits_are_supported(self):
        import short_scope
        cfg = configuration(); cfg.update(short_scope.expected('2026-10-19'))
        supervisor.validate(cfg)
        self.assertEqual(cfg['earliest_entry_utc'], short_scope.epoch('2026-10-19', 9))
        self.assertEqual(cfg['hard_cleanup_deadline_utc'], short_scope.epoch('2026-10-19', 15))


class CITests(unittest.TestCase):
    def setUp(self):
        self.cfg, self.writes, self.calls = configuration(), [], []
        review = self.cfg["review_git_sha"]
        self.data = {
            "actions/runs/" + str(runtime.CI_RUN_ID): dict(id=runtime.CI_RUN_ID, head_sha=runtime.HEAD, status="completed", conclusion="success"),
            "actions/runs/123": dict(id=123, head_sha=review, status="completed", conclusion="success"),
            "pulls/68": dict(state="open", draft=True, head=dict(sha=review)),
            "compare/" + runtime.HEAD + "..." + review: dict(merge_base_commit=dict(sha=runtime.HEAD), status="ahead", total_commits=1,
                files=[dict(filename="scripts/test_rehearsal/short_session.py", status="added"), dict(filename="CERTIFICATION_DAY_RUNBOOK.md", status="modified")]),
            "commits/" + review + "/check-runs": dict(check_runs=[dict(id=100+index,head_sha=review,name=name, status="completed", conclusion="success") for index,name in enumerate(sorted(runtime.JOBS))]),
        }
        def fetch(suffix): self.calls.append(suffix); return self.data[suffix]
        self.fetch = patch.object(runtime, "fetch", side_effect=fetch); self.fetch.start()
        self.atomic = patch.object(runtime_base, "atomic", side_effect=lambda path, data:self.writes.append((path, data))); self.atomic.start()
    def tearDown(self): self.atomic.stop(); self.fetch.stop()
    def comparison(self): return self.data["compare/" + runtime.HEAD + "..." + self.cfg["review_git_sha"]]
    def test_review_head_may_differ_from_native_but_both_exact_ci_pins_pass(self):
        runtime.verify_ci_for_seal(Path("test-root"), self.cfg)
        self.assertNotEqual(self.cfg["review_git_sha"],runtime.HEAD)
        self.assertEqual(len(self.writes),1)
        public=self.writes[0][1]
        self.assertEqual(public["source_git_sha"],runtime.HEAD)
        self.assertEqual(public["review_git_sha"],self.cfg["review_git_sha"])
        self.assertEqual(public["native_ci_run_id"],runtime.CI_RUN_ID)
        self.assertEqual(public["review_ci_run_id"],123)
    def test_native_or_renamed_or_unapproved_file_refuses(self):
        for filename, status in (("connectors/connector_host/src/trading_host.cpp","modified"),
                                 ("CERTIFICATION.md","renamed"),("README.md","modified"),
                                 ("scripts/test_rehearsalx/escape.py","added")):
            self.comparison()["files"]=[dict(filename=filename,status=status)]
            with self.subTest(filename=filename,status=status),self.assertRaisesRegex(Refused,"native_or_unapproved"):
                runtime.verify_ci_for_seal(Path("test-root"),self.cfg)
        self.assertEqual(self.writes,[])
    def test_comparison_must_be_complete_ancestor_and_bounded(self):
        original=copy.deepcopy(self.comparison())
        for change in (dict(merge_base_commit=dict(sha="e"*40)),dict(status="diverged"),dict(total_commits=251),dict(total_commits=True),
                       dict(files=[dict(filename="CERTIFICATION.md",status="modified")]*300)):
            current=self.comparison();current.clear();current.update(copy.deepcopy(original));current.update(change)
            with self.subTest(change=list(change)),self.assertRaisesRegex(Refused,"comparison_incomplete"):
                runtime.verify_ci_for_seal(Path("test-root"),self.cfg)
        self.assertEqual(self.writes,[])
    def test_native_pin_and_review_run_cannot_be_substituted(self):
        for change in (dict(ci_run_id=123),dict(source_git_sha="f"*40),dict(review_git_sha="short")):
            with self.subTest(change=change),self.assertRaises(Refused):
                runtime.verify_ci_for_seal(Path("test-root"),dict(self.cfg,**change))
        self.data["actions/runs/123"]["head_sha"]=runtime.HEAD
        with self.assertRaisesRegex(Refused,"review_exact_ci"):
            runtime.verify_ci_for_seal(Path("test-root"),self.cfg)
        self.assertEqual(self.writes,[])
    def test_each_exact_run_and_both_latest_jobs_must_be_green(self):
        for suffix in ("actions/runs/"+str(runtime.CI_RUN_ID),"actions/runs/123"):
            self.data[suffix]["conclusion"]="failure"
            with self.subTest(suffix=suffix),self.assertRaisesRegex(Refused,"exact_ci"):
                runtime.verify_ci_for_seal(Path("test-root"),self.cfg)
            self.data[suffix]["conclusion"]="success"
        checks=self.data["commits/"+self.cfg["review_git_sha"]+"/check-runs"]["check_runs"]
        checks[0]["status"]="in_progress"
        with self.assertRaisesRegex(Refused,"both_ci_jobs"):
            runtime.verify_ci_for_seal(Path("test-root"),self.cfg)
        checks.pop(0)
        with self.assertRaisesRegex(Refused,"both_ci_jobs"):
            runtime.verify_ci_for_seal(Path("test-root"),self.cfg)
        self.assertEqual(self.writes,[])
    def test_pr_must_remain_open_draft_at_exact_review_head(self):
        for change in (dict(state="closed"),dict(draft=False),dict(head=dict(sha=runtime.HEAD))):
            original=copy.deepcopy(self.data["pulls/68"]);self.data["pulls/68"].update(change)
            with self.subTest(change=change),self.assertRaisesRegex(Refused,"review_head"):
                runtime.verify_ci_for_seal(Path("test-root"),self.cfg)
            self.data["pulls/68"]=original
        self.assertEqual(self.writes,[])

    def test_truncated_http_payload_retries_and_rechecks_all_exact_ci_gates(self):
        self.fetch.stop()
        calls=[];truncated=[False]
        class Response:
            def __init__(response,suffix):response.suffix=suffix
            def __enter__(response):return response
            def __exit__(response,*args):return False
            def read(response):
                if response.suffix=='pulls/68' and not truncated[0]:
                    truncated[0]=True
                    raise runtime.http.client.IncompleteRead(b'{"state":',100)
                return json.dumps(self.data[response.suffix]).encode()
        def open_response(request,timeout):
            suffix=request.full_url.split('/repos/Ruhkukah/Connectors/',1)[1]
            calls.append(suffix)
            return Response(suffix)
        with patch.object(runtime.urllib.request,'urlopen',side_effect=open_response),patch.object(runtime.time,'sleep'),patch.object(runtime.time,'time',return_value=supervisor.epoch(9)):
            runtime.verify_ci_for_seal(Path('no-native'),self.cfg)
        native='actions/runs/'+str(runtime.CI_RUN_ID)
        self.assertEqual(calls[:3],[native,'actions/runs/123','pulls/68'])
        self.assertEqual(calls[3:],[native,'actions/runs/123','pulls/68',
                         'compare/'+runtime.HEAD+'...'+self.cfg['review_git_sha'],
                         'commits/'+self.cfg['review_git_sha']+'/check-runs'])
        self.assertEqual(len(self.writes),1)
        self.assertTrue(self.writes[0][1]['scripts_and_documentation_only'])
        self.assertEqual(self.writes[0][1]['review_git_sha'],self.cfg['review_git_sha'])

    def test_complete_but_malformed_json_is_not_transport_retry(self):
        self.fetch.stop()
        class Response:
            def __enter__(response):return response
            def __exit__(response,*args):return False
            def read(response):return b'{"complete_but_invalid_json":'
        with patch.object(runtime.urllib.request,'urlopen',return_value=Response()) as opened,patch.object(runtime.time,'sleep') as slept:
            with self.assertRaises(json.JSONDecodeError):runtime.verify_ci_for_seal(Path('no-native'),self.cfg)
        self.assertEqual(opened.call_count,1);slept.assert_not_called()
        self.assertEqual(self.writes,[])

    def test_completed_payload_with_wrong_exact_pin_stays_fatal_no_retry(self):
        self.data['actions/runs/'+str(runtime.CI_RUN_ID)]['head_sha']='f'*40
        with patch.object(runtime.time,'sleep') as slept:
            with self.assertRaisesRegex(Refused,'native_exact_ci'):
                runtime.verify_ci_for_seal(Path('no-native'),self.cfg)
        self.assertEqual(self.calls,['actions/runs/'+str(runtime.CI_RUN_ID)])
        slept.assert_not_called();self.assertEqual(self.writes,[])


class SealedReceiptTests(unittest.TestCase):
    setUp = CITests.setUp
    tearDown = CITests.tearDown
    def seal(self, root):
        runtime.verify_ci_for_seal(root,self.cfg)
        receipt=self.writes[-1][1]
        self.write_receipt(root,receipt)
        return receipt
    def write_receipt(self,root,receipt,pinned=True):
        path=root/'ci-public.json';path.write_text(json.dumps(receipt))
        files={'ci-public.json':hashlib.sha256(path.read_bytes()).hexdigest()} if pinned else {}
        (root/'manifest.json').write_text(json.dumps(dict(files=files)))
    def offline(self,root):
        with patch.object(runtime_base,'sha',side_effect=lambda path:hashlib.sha256(Path(path).read_bytes()).hexdigest(),create=True):
            return runtime.verify_ci(root,self.cfg)
    def test_preflight_uses_manifest_pinned_exact_receipt_without_network(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);self.seal(root);before=list(self.calls)
            with patch.object(runtime,'fetch',side_effect=AssertionError('no preflight network')):
                result=self.offline(root)
            self.assertEqual(result['binary_sha256'],runtime.BINARY)
            self.assertEqual(result['native_run']['id'],runtime.CI_RUN_ID)
            self.assertEqual(result['review_run']['id'],self.cfg['review_ci_run_id'])
            self.assertEqual(set(result['review_checks']),runtime.JOBS)
            self.assertEqual(self.calls,before)
    def test_unpinned_or_changed_ci_receipt_cannot_grant_preflight(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);receipt=self.seal(root)
            self.write_receipt(root,receipt,pinned=False)
            with self.assertRaisesRegex(Refused,'unpinned_or_changed'):self.offline(root)
            self.write_receipt(root,receipt)
            (root/'ci-public.json').write_text('{}')
            with self.assertRaisesRegex(Refused,'unpinned_or_changed'):self.offline(root)
    def test_pinned_but_mismatched_receipt_metadata_or_jobs_refuses(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);original=self.seal(root)
            mutations=[lambda r:r.update(source_git_sha='f'*40),lambda r:r.update(binary_sha256='f'*64),
                lambda r:r.update(review_ci_run_id=999),lambda r:r['native_run'].update(conclusion='failure'),
                lambda r:r['protected_pr'].update(head_sha='f'*40),lambda r:r.update(review_checks={})]
            for mutate in mutations:
                receipt=copy.deepcopy(original);mutate(receipt);self.write_receipt(root,receipt)
                with self.assertRaises(Refused):self.offline(root)
    def test_pinned_native_file_in_comparison_still_refuses(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);receipt=self.seal(root)
            paths=[dict(filename='native.cpp',status='modified')]
            receipt['comparison']['files']=paths
            receipt['comparison']['files_sha256']=hashlib.sha256(json.dumps(paths,sort_keys=True,separators=(',',':')).encode()).hexdigest()
            self.write_receipt(root,receipt)
            with self.assertRaisesRegex(Refused,'native_or_unapproved'):self.offline(root)


class CleanupOrderingTests(unittest.TestCase):
    def runner(self, pending=True):
        runner=supervisor.Supervisor();runner.events=[];runner.public=[];runner.cfg=configuration()
        runner.verify_owner=lambda _:(runner.events.append("restored_owner") or dict(client_count=1,upstreams=4))
        runner.product=types.SimpleNamespace(status=lambda:{'complete':True})
        runner.targets=[];runner.tick=lambda:None
        runner.ops=types.SimpleNamespace(ready=lambda *a,**k:True)
        runner.rt=types.SimpleNamespace(configuration_gate=lambda *a:None)
        runner.publish=runner.public.append
        if pending:
            controller=types.SimpleNamespace(fault_worker=321,guard_control=42,
                fault_lock=types.SimpleNamespace(close=lambda:runner.events.append("unlock")))
            def finish():runner.events.append("restore_wait");controller.fault_worker=None
            controller.wait_fault_worker=finish
            runner.pending_fault_controller=controller
        return runner
    def test_restore_worker_finishes_before_pipe_unlock_and_product_cleanup(self):
        runner=self.runner()
        with patch.object(supervisor.os,"close",side_effect=lambda _:runner.events.append("close_pipe")):
            self.assertEqual(runner.fail(RuntimeError("original")),"cleanup_result")
        self.assertEqual(runner.events,["restore_wait","restored_owner","close_pipe","unlock","base_cleanup"])
        self.assertFalse(hasattr(runner,"pending_fault_controller"))
    def test_pending_restore_waits_then_cleans_after_actual_restoration(self):
        runner=self.runner(); calls=[]
        def wait():
            calls.append('wait')
            if len(calls)==1: raise RuntimeError('still restoring')
            runner.pending_fault_controller.fault_worker=None
        runner.pending_fault_controller.wait_fault_worker=wait
        runner.tick=lambda:None
        with patch.object(supervisor.os,'close'), patch.object(supervisor.time,'sleep'):
            runner.fail(RuntimeError('case assertion'))
        self.assertEqual(calls,['wait','wait'])
        self.assertIn('base_cleanup', runner.events)

    def test_restore_deadline_retains_lock_and_publishes_unknown(self):
        runner=self.runner(); runner.cfg['hard_cleanup_deadline_utc']=0
        runner.pending_fault_controller.wait_fault_worker=lambda: (_ for _ in ()).throw(RuntimeError('pending'))
        runner.emergency_cancel=lambda:runner.events.append('actual_cancel_attempt')
        with patch.object(supervisor.os,'close') as close:
            runner.fail(RuntimeError('initiating'))
        close.assert_not_called()
        self.assertIn('actual_cancel_attempt',runner.events)
        self.assertTrue(hasattr(runner,'pending_fault_controller'))

    def test_failed_worker_already_exited_waits_for_watchdog_and_full_native_then_cleans(self):
        runner=self.runner(False);runner.fault_restore_required=True;attempts=[]
        def inventory(product):
            attempts.append('scan')
            if len(attempts)==1:raise Refused('router_still_restoring')
            return dict(client_count=1,upstreams=4)
        runner.verify_owner=inventory
        snapshots=iter([False,True])
        runner.product.status=lambda:dict(complete=next(snapshots))
        runner.ops.ready=lambda state,*a,**k:state['complete']
        with patch.object(supervisor.time,'sleep'):
            self.assertEqual(runner.fail(RuntimeError('helper_exit_2')),'cleanup_result')
        self.assertEqual(attempts,['scan','scan','scan'])
        self.assertFalse(runner.fault_restore_required)
        self.assertEqual(runner.events,['base_cleanup'])

    def test_worker_exit_does_not_clear_restore_flag_without_four_upstreams(self):
        runner=self.runner(False);runner.fault_restore_required=True
        runner.cfg['hard_cleanup_deadline_utc']=supervisor.epoch(10)+60
        runner.verify_owner=lambda _:dict(client_count=1,upstreams=3)
        runner.emergency_cancel=lambda:runner.events.append('actual_cancel_attempt')
        with patch.object(supervisor.time,'time',side_effect=[supervisor.epoch(10),supervisor.epoch(10),supervisor.epoch(10)+50]):
            runner.fail(RuntimeError('worker_exited'))
        self.assertTrue(runner.fault_restore_required)
        self.assertIn('actual_cancel_attempt',runner.events)
        self.assertNotIn('base_cleanup',runner.events)

    def test_no_pending_fault_uses_existing_owned_cleanup(self):
        runner=self.runner(False)
        self.assertEqual(runner.fail(RuntimeError()),"cleanup_result")
        self.assertEqual(runner.events,["base_cleanup"])


class PreintentTests(unittest.TestCase):
    def runner(self, states):
        runner=supervisor.Supervisor(); runner.cfg=configuration(); runner.closing=False
        runner.work=Path('/nonexistent-test-root'); runner.product=types.SimpleNamespace(status=lambda:states.pop(0))
        runner.events=[]; runner.targets=[dict(isin_id=1,quote_cap_scaled=20),dict(isin_id=2,quote_cap_scaled=20)]
        runner.verify_owner=lambda p:runner.events.append('owner')
        runner._cheap_owner=lambda p:runner.events.append('cheap_owner')
        runner.rt=types.SimpleNamespace(day_check=lambda *a,**k:None,
            configuration_gate=lambda *a:runner.events.append('configuration'))
        runner.ops=types.SimpleNamespace(ready=lambda state,targets,entry=False:state['complete'])
        return runner
    def state(self, complete=True, ready=(True,True)):
        return dict(complete=complete,instruments=[dict(isin_id=i+1,order_entry_ready=r,book=dict(valid=r)) for i,r in enumerate(ready)])
    def test_selected_book_may_be_ready_while_other_book_one_sided_but_all_streams_complete(self):
        state=self.state(ready=(True,False));runner=self.runner([state])
        prices=types.SimpleNamespace(current_reference=lambda state,target:10)
        with patch.dict(sys.modules,{'price_caps':prices}),patch.object(supervisor.time,'time',return_value=supervisor.epoch(10)):
            self.assertIs(runner.risk_snapshot(runner.product,[runner.targets[0]]),state)
        self.assertEqual(runner.events.count('owner'),1)
        self.assertEqual(runner.events.count('cheap_owner'),1)
    def test_incomplete_account_reconstruction_waits_on_one_snapshot_per_iteration(self):
        state=self.state(); runner=self.runner([self.state(complete=False),state]);reads=[]
        current=runner.product.status
        runner.product.status=lambda:(reads.append('status') or current())
        prices=types.SimpleNamespace(current_reference=lambda state,target:10)
        with patch.dict(sys.modules,{'price_caps':prices}),patch.object(supervisor.time,'time',return_value=supervisor.epoch(10)),patch.object(supervisor.time,'sleep'):
            self.assertIs(runner.risk_snapshot(runner.product),state)
        self.assertEqual(reads,['status','status'])
        self.assertEqual(runner.events.count('owner'),1)
    def test_stale_book_waits_before_intent_without_repeating_dispatch(self):
        state=self.state();runner=self.runner([state,state]); bids=iter([ValueError('current_price_book_stale'),10,10])
        def bid(*a):
            value=next(bids)
            if isinstance(value,Exception):raise value
            return value
        with patch.dict(sys.modules,{'price_caps':types.SimpleNamespace(current_reference=bid)}),patch.object(supervisor.time,'time',return_value=supervisor.epoch(10)),patch.object(supervisor.time,'sleep'):
            self.assertIs(runner.risk_snapshot(runner.product),state)
        self.assertEqual(runner.events.count('owner'),1)
    def test_expired_wait_is_typed_not_sent(self):
        runner=self.runner([]);start=supervisor.epoch(10)
        with patch.dict(sys.modules,{'price_caps':types.SimpleNamespace()}),patch.object(supervisor.time,'time',side_effect=[start,start,start+31]):
            with self.assertRaises(supervisor.PreIntentNotSent) as caught:runner.risk_snapshot(runner.product)
        self.assertIs(caught.exception.not_sent,True)
    def test_fast_dispatch_preflights_once_then_keeps_cheap_identity_date_guard(self):
        runner=self.runner([]);preflight=[]
        runner.risk_snapshot=lambda *a,**k:preflight.append(k['selected'])
        with patch.object(supervisor.time,'time',return_value=supervisor.epoch(10)):
            with runner.fast_dispatch(runner.product,runner.targets):
                for _ in range(30):runner.risk_gate(runner.product)
        self.assertEqual(preflight,[runner.targets]);self.assertEqual(runner.events.count('cheap_owner'),30)
        self.assertIsNone(runner._fast_product)
    def test_uncertain_native_dispatch_is_not_relabeled_not_sent(self):
        class Stop(RuntimeError):pass
        class RawProduct:
            def command(self,line,permit_risk=True):
                if self.risk_guard:self.risk_guard()
                self.intents+=1
                raise Stop('product_command_refused_or_uncertain')
        runner=self.runner([]);runner.ops=types.SimpleNamespace(Product=RawProduct)
        product=runner.product_class()();product.risk_guard=lambda:None;product.intents=0
        runner.product=product;runner.verify_owner=lambda p:None;runner.publish=lambda p:None
        with self.assertRaises(Stop) as caught:product.command('place c 1 buy 1 1 day')
        self.assertFalse(getattr(caught.exception,'not_sent',False));self.assertEqual(product.intents,1)
    def test_uncertain_or_reconciliation_mismatch_never_qualifies_for_case_continue(self):
        for error in (RuntimeError('command_outcome_uncertain'),RuntimeError('short_fault_owned_order_or_position_changed'),RuntimeError('reconciliation_mismatch'),RuntimeError('working_order_or_fill_baseline_changed'),RuntimeError('position_baseline_changed')):
            self.assertTrue(supervisor.uncertain_or_mismatched(error))
        self.assertFalse(supervisor.uncertain_or_mismatched(supervisor.PreIntentNotSent('not_sent')))

    def test_case_failure_continues_only_after_cancel_flat_and_strict_router_proof(self):
        runner=self.runner([]);runner.product=object();runner.meta={}
        state=self.state();state['flat']=True
        runner.product=types.SimpleNamespace(status=lambda:state)
        runner.ops.flat=lambda state:state['flat']
        runner._wait_fault_restoration=lambda:runner.events.append('restored')
        runner.rt.verify_owner=lambda meta:dict(client_count=1,upstreams=4)
        runner.flatten=lambda product,deadline,final=False:(runner.events.append('cancel_flat') or dict(cancel_confirmed=True,flat=True))
        with patch.object(supervisor.time,'time',return_value=supervisor.epoch(10)):
            proof=runner.safe_case_failure(runner.product,RuntimeError('scenario_evidence_missing'),'case')
        self.assertEqual(proof,dict(cancel_confirmed=True,flat=True,restored=True,upstreams=4,
                                   client_count=1,complete_reconstruction=True,failed_case='case'))
        self.assertEqual(runner.events[:3],['restored','owner','cancel_flat'])
        for bad in (dict(client_count=2,upstreams=4),dict(client_count=1,upstreams=3)):
            runner.rt.verify_owner=lambda meta,value=bad:value
            with patch.object(supervisor.time,'time',return_value=supervisor.epoch(10)):
                with self.assertRaisesRegex(Refused,'router_restoration'):
                    runner.safe_case_failure(runner.product,RuntimeError('scenario_evidence_missing'),'case')
    def test_uncertain_or_unrestored_cleanup_epoch_never_clears_request_latches(self):
        with tempfile.TemporaryDirectory() as folder:
            for confirmed,upstreams in ((False,4),(True,3)):
                runner=self.runner([]);runner.root=Path(folder);runner.meta={}
                runner.product=types.SimpleNamespace(status=lambda:dict(complete=True,flat=True))
                runner.ops.flat=lambda state:state['flat']
                runner._wait_fault_restoration=lambda:None
                runner.rt.verify_owner=lambda meta,n=upstreams:dict(client_count=1,upstreams=n)
                runner.rt.atomic=lambda *a:self.fail('unconfirmed cleanup cannot archive/rearm')
                runner.flatten=lambda *a,**k:dict(cancel_confirmed=True,flat=True)
                attempts={1:dict(confirmed=confirmed,command_outcome='UNCERTAIN_NEVER_RETRIED')}
                runner.emergency_cancel_attempts=attempts
                with patch.object(supervisor.time,'time',return_value=supervisor.epoch(10)):
                    with self.assertRaises(Refused):runner.safe_case_failure(runner.product,RuntimeError('case'),'case')
                self.assertIs(runner.emergency_cancel_attempts,attempts)
                self.assertFalse((runner.root/'emergency-cleanup-epoch-0-public.json').exists())

    def test_uncertain_case_failure_never_executes_a_new_case(self):
        runner=self.runner([]);runner._wait_fault_restoration=lambda:None
        runner.flatten=lambda *a,**k:self.fail('uncertain case cannot qualify for continuation')
        with self.assertRaisesRegex(Refused,'unsafe_case_failure'):
            runner.safe_case_failure(runner.product,RuntimeError('command_outcome_uncertain'),'case')


class EmergencyCleanupTests(unittest.TestCase):
    def test_three_upstreams_still_attempts_both_actual_native_cancel_scopes(self):
        with tempfile.TemporaryDirectory() as folder:
            runner=supervisor.Supervisor();runner.work=Path(folder);runner.root=Path(folder)
            journal=runner.work/'interaction.ndjson';journal.write_bytes(b'')
            runner.cfg=configuration();runner.public=[];runner.publish=runner.public.append
            identity=dict(pid=9,start_ticks='123',executable='/verified-binary')
            product=types.SimpleNamespace(child=types.SimpleNamespace(pid=9,poll=lambda:None),
                binary='/verified-binary',args=['/verified-binary','--command-socket',str(runner.work/'cmd.sock')],env={})
            runner.product=runner.latest_native_product=product;runner.latest_native_identity=identity
            runner.targets=[dict(isin_id=1,symbol='ALRS-12.26'),dict(isin_id=2,symbol='CNY-12.26')]
            runner.ops=types.SimpleNamespace(process_identity=lambda pid:identity,ready=lambda *a,**k:True,flat=lambda s:True,
                lock=lambda path,nonblocking=False:path.open('a'))
            runner.rt=types.SimpleNamespace(sha=lambda path:super_base.BINARY,configuration_gate=lambda *a:None,
                atomic=lambda path,value:Path(path).write_text(json.dumps(value)))
            runner.verify_owner=lambda p:(_ for _ in ()).throw(Refused('three_upstreams'))
            calls=[]
            def actual_cli(args,**kwargs):
                line=args[-1];calls.append(line)
                if line.startswith('cancel-all '):
                    isin=int(line.split()[1]);user=100+isin
                    events=[dict(event='operator_input',data=dict(line=line,channel='command_socket')),
                        dict(event='command',data=dict(name='DelUserOrders',user_id=user,fields=dict(isin_id=isin))),
                        dict(event='reply',data=dict(user_id=user,msgid=186,code=0)),
                        dict(event='transaction_commit',data=dict(stream='FORTS_TRADE_REPL'))]
                    with journal.open('a') as f:
                        for event in events:f.write(json.dumps(event)+'\n')
                    if isin == 1:
                        raise supervisor.subprocess.TimeoutExpired(args,10)
                    return types.SimpleNamespace(returncode=0,stdout=b'{"ok":true}')
                return types.SimpleNamespace(returncode=0,stdout=b'{"native":"complete-flat"}')
            with patch.object(supervisor.subprocess,'run',side_effect=actual_cli),patch.object(supervisor.time,'time',return_value=supervisor.epoch(10)):
                proof=runner.emergency_cancel()
                repeat=runner.emergency_cancel()
            self.assertEqual(calls.count('cancel-all 1'),1);self.assertEqual(calls.count('cancel-all 2'),1)
            self.assertTrue(proof['cancel_confirmed']);self.assertTrue(proof['flat'])
            self.assertEqual(proof['attempts'][0]['command_outcome'],'UNCERTAIN_NEVER_RETRIED')
            self.assertTrue(repeat['no_new_native_process']);self.assertEqual(runner.public[-1]['state'],'BLOCKED')
            self.assertFalse(runner.public[-1]['terminal'])
            # A new case may place new orders only after strict cancellation,
            # flatness, complete reconstruction and router/client reconciliation.
            runner.meta={};runner.closing=False
            product.status=lambda:dict(native='complete-flat')
            runner._wait_fault_restoration=lambda:None
            runner.verify_owner=lambda p:dict(client_count=1,upstreams=4)
            runner.rt.verify_owner=lambda meta:dict(client_count=1,upstreams=4)
            runner.flatten=lambda *a,**k:dict(cancel_confirmed=True,flat=True)
            with patch.object(supervisor.time,'time',return_value=supervisor.epoch(10)):
                runner.safe_case_failure(product,RuntimeError('recoverable_case_evidence_missing'),'first-case')
            self.assertEqual(runner.emergency_cancel_attempts,{})
            archived=json.loads((runner.root/'emergency-cleanup-epoch-0-public.json').read_text())
            self.assertEqual(archived['state'],'CLOSED_CONFIRMED_SAFE')
            self.assertEqual(len(archived['attempts']),2)
            self.assertTrue(all(row['confirmed'] for row in archived['attempts']))
            with patch.object(supervisor.subprocess,'run',side_effect=actual_cli),patch.object(supervisor.time,'time',return_value=supervisor.epoch(10)):
                later=runner.emergency_cancel()
            self.assertTrue(later['cancel_confirmed'])
            self.assertEqual(calls.count('cancel-all 1'),2);self.assertEqual(calls.count('cancel-all 2'),2)
    def test_foreign_cleanup_lock_prevents_duplicate_cancel_and_keeps_unknown(self):
        with tempfile.TemporaryDirectory() as folder:
            runner=supervisor.Supervisor();runner.work=runner.root=Path(folder);runner.public=[];runner.publish=runner.public.append
            identity=dict(pid=9,start_ticks='123',executable='/candidate')
            runner._emergency_owner=lambda:(object(),identity)
            locks=[]
            def held(path,nonblocking=False):
                locks.append((path,nonblocking));raise BlockingIOError('another cleanup owner')
            runner.ops=types.SimpleNamespace(lock=held)
            runner.rt=types.SimpleNamespace(atomic=lambda path,data:Path(path).write_text(json.dumps(data)))
            with patch.object(supervisor.subprocess,'run') as dispatched:
                proof=runner.emergency_cancel()
            dispatched.assert_not_called()
            self.assertEqual(locks,[(runner.work/'cleanup.lock',True)])
            self.assertEqual(proof['flat'],'UNKNOWN');self.assertTrue(proof['no_duplicate_cancel_sent'])
            self.assertEqual(json.loads((runner.root/'emergency-product-owned-private.json').read_text()),identity)
    def test_fatal_failure_dispatches_emergency_cancel_before_restore_wait(self):
        runner=supervisor.Supervisor();runner.product=object();runner.meta={};runner.events=[];runner.public=[]
        runner.publish=runner.public.append
        runner.emergency_cancel=lambda:(runner.events.append('cancel_both_now') or dict(cancel_confirmed=True,flat=True))
        runner._wait_fault_restoration=lambda:runner.events.append('wait_restore')
        runner.fail(RuntimeError('fault_case_failed'))
        self.assertEqual(runner.events,['cancel_both_now','wait_restore','base_cleanup'])
    def test_unattached_verified_child_flat_then_single_SIGTERM_no_new_boot(self):
        runner=supervisor.Supervisor();runner.root=Path('private-root');runner.cfg=configuration();runner.environment={}
        runner.shutdown_sent=False;runner.public=[];runner.publish=runner.public.append;events=[]
        identity=dict(pid=9,start_ticks='123',executable='/candidate')
        child=types.SimpleNamespace(returncode=None)
        def wait(timeout):events.append('wait_exit');child.returncode=0
        child.wait=wait;product=types.SimpleNamespace(child=child)
        runner._emergency_owner=lambda:(product,identity)
        runner.rt=types.SimpleNamespace(metadata=lambda *a:dict(identity=identity),
            atomic=lambda path,value:events.append('save_owned_metadata'),
            owned_sigterm_once=lambda meta:events.append('single_SIGTERM'),
            inventory=lambda meta:dict(client_count=0,upstreams=4))
        with patch.object(supervisor.time,'time',return_value=supervisor.epoch(10)),patch.object(supervisor.subprocess,'Popen') as started:
            runner._stop_unattached_after_flat(dict(cancel_confirmed=True,flat=True))
        started.assert_not_called();self.assertEqual(events,['save_owned_metadata','single_SIGTERM','wait_exit'])
        self.assertEqual(runner.public[-1]['state'],'BLOCKED');self.assertFalse(runner.public[-1]['terminal'])
        self.assertEqual(runner.public[-1]['emergency_replacement_closeout']['product_exit'],0)
        self.assertEqual(runner.public[-1]['emergency_replacement_closeout']['after']['client_count'],0)

    def test_started_child_identity_survives_metadata_write_failure_without_second_boot(self):
        with tempfile.TemporaryDirectory() as folder:
            runner=supervisor.Supervisor();child=types.SimpleNamespace(pid=77,poll=lambda:None)
            identity=dict(pid=77,start_ticks='91',executable='/candidate')
            class RawProduct:pass
            popen=types.SimpleNamespace()
            with patch.object(supervisor.subprocess,'Popen',return_value=child) as started:
                runner.ops=types.SimpleNamespace(Product=RawProduct,sha=lambda path:'same',
                    subprocess=supervisor.subprocess,process_identity=lambda pid:identity,
                    atomic=lambda *a:(_ for _ in ()).throw(OSError('metadata write failed')))
                product=runner.product_class()();product.work=Path(folder);product.binary='/candidate'
                product.args=['/candidate','--command-socket',str(product.work/'cmd.sock')]
                product.env={};product.expected_sha='same'
                with self.assertRaisesRegex(OSError,'metadata'):product.start()
            self.assertIs(runner.latest_native_product,product)
            self.assertEqual(runner.latest_native_identity,identity)
            self.assertEqual(started.call_count,1)

    def test_stale_empty_scope_and_nonzero_reply_are_not_confirmation(self):
        def row(kind,data,begin):return dict(begin=begin,end=begin+1,event=dict(event=kind,data=data))
        empty=row('cancel_all_empty',dict(isin_id=1,snapshot_confirmed=True),0)
        self.assertFalse(supervisor.cancel_scope_proof([empty],1,True)['confirmed'])
        requested=row('operator_input',dict(line='cancel-all 1',channel='command_socket'),1)
        command=row('command',dict(name='DelUserOrders',user_id=8,fields=dict(isin_id=1)),2)
        reply=row('reply',dict(user_id=8,msgid=186,code=1),3)
        commit=row('transaction_commit',dict(stream='FORTS_TRADE_REPL'),4)
        self.assertFalse(supervisor.cancel_scope_proof([requested,command,reply,commit],1,True)['confirmed'])
    def test_no_product_after_planned_crash_alarms_without_third_start(self):
        runner=supervisor.Supervisor();runner.public=[];runner.publish=runner.public.append
        runner.recovery_needed=True;runner.meta=dict(identity=dict(pid=1));runner.last_restart_working_count=2
        runner.product=types.SimpleNamespace(child=types.SimpleNamespace(poll=lambda:-9))
        runner._wait_fault_restoration=lambda:None
        runner.emergency_cancel=lambda:(_ for _ in ()).throw(Refused('no_owned_alive_product'))
        runner.fail(RuntimeError('replacement_start_failed'))
        final=runner.public[-1]
        self.assertEqual(final['phase'],'orders_working_no_product');self.assertEqual(final['last_known_working_order_count'],2)
        self.assertTrue(final['no_third_native_boot_authorized']);self.assertEqual(final['flat'],'UNKNOWN')
    def test_nonflat_complete_preflight_probe_fails_immediately_without_retry(self):
        with tempfile.TemporaryDirectory() as folder:
            runner=supervisor.Supervisor();runner.root=Path(folder)
            probe=runner.root/'before-native-independent-flat';probe.mkdir()
            (probe/'snapshot-observation-public.json').write_text(json.dumps(dict(exit=5,
                fresh_private_snapshot_ready=True,working_orders=1,nonzero_positions=0)))
            runner._preflight_pause=lambda:self.fail('actual exposure cannot be a preflight retry')
            with patch.object(ParentSupervisor,'initial_flat_probe',side_effect=Refused('fresh_independent_flatness_unproved'),create=True) as checked:
                with self.assertRaisesRegex(Refused,'actual_nonflat_account'):runner.initial_flat_probe()
            self.assertEqual(checked.call_count,1);self.assertTrue(probe.exists())
    def test_failed_case_can_never_publish_complete_qualification(self):
        result=dict(terminal=True,execution_result='PASS',qualification_result='SUPPLEMENTAL_TEST_COMPLETE',
                    scenarios=dict(first=dict(status='PASS'),second=dict(status='FAIL')))
        self.assertEqual(supervisor.supplemental_qualification(result),'SUPPLEMENTAL_TEST_INCOMPLETE')
        result['scenarios']['second']['status']='PASS'
        self.assertEqual(supervisor.supplemental_qualification(result),'SUPPLEMENTAL_TEST_COMPLETE')


class CanonicalHelperTests(unittest.TestCase):
    def test_historical_probe_and_rollback_use_identical_refused_class(self):
        events=[];other=types.ModuleType('other_router');other.Refused=type('Refused',(Exception,),{})
        canonical=types.ModuleType('router_recovery');canonical.Refused=type('Refused',(Exception,),{});canonical.__file__='router'
        upstream=types.ModuleType('upstream_recovery');upstream.__file__='upstream'
        def install(router,helper,**kwargs):
            import router_recovery
            self.assertIs(router_recovery,canonical)
            self.assertIs(router.Refused,router_recovery.Refused)
            events.append('installed')
        helpers=types.SimpleNamespace(install=install)
        def load(filename,name):
            events.append(name)
            value={'router_recovery':canonical,'upstream_recovery':upstream,'short_fault_helpers':helpers}[name]
            sys.modules[name]=value
            return value
        cfg=dict(configuration(),router_helper_sha256='r',upstream_helper_sha256='u')
        rt=types.SimpleNamespace(sha=lambda path:{'router':'r','upstream':'u'}[path])
        with patch.dict(sys.modules,{'other_router':other}):
            router,_=supervisor.load_fault_helpers(load,rt,cfg)
            import router_recovery
            try:raise router_recovery.Refused('router_not_bound_yet')
            except router.Refused:events.append('bounded_retry')
        self.assertEqual(events,['router_recovery','upstream_recovery','short_fault_helpers','installed','bounded_retry'])
        self.assertIsNot(other.Refused,canonical.Refused)


class PreflightRetryTests(unittest.TestCase):
    def test_network_hiccup_retries_without_any_native_process(self):
        cfg=configuration();calls=[]
        def check(*args):
            calls.append('read_only_ci')
            if len(calls)==1:raise runtime.urllib.error.URLError('temporary')
        with patch.object(runtime,'_verify_ci_once',side_effect=check),patch.object(runtime.time,'sleep'),patch.object(runtime.time,'time',return_value=supervisor.epoch(9)):
            runtime.verify_ci_for_seal(Path('no-native'),cfg)
        self.assertEqual(calls,['read_only_ci','read_only_ci'])
    def test_semantic_ci_pin_failure_never_retries(self):
        with patch.object(runtime,'_verify_ci_once',side_effect=Refused('review_head_changed')) as check:
            with self.assertRaisesRegex(Refused,'review_head'):runtime.verify_ci_for_seal(Path('no-native'),configuration())
        self.assertEqual(check.call_count,1)
    def test_http_forbidden_fatal_but_rate_limit_and_server_failure_transient(self):
        for code,transient in ((403,False),(404,False),(429,True),(503,True)):
            error=runtime.urllib.error.HTTPError('https://example.invalid',code,'error',{},None)
            self.assertEqual(runtime.transient_network(error),transient)

    def test_ssl_body_read_error_and_explicit_rate_limited_403_are_transient(self):
        self.assertTrue(runtime.transient_network(runtime.ssl.SSLError('body read failed')))
        limited=runtime.urllib.error.HTTPError('https://example.invalid',403,'limit',{'X-RateLimit-Remaining':'0'},None)
        self.assertTrue(runtime.transient_network(limited))
        self.assertFalse(runtime.transient_network(runtime.urllib.error.HTTPError('https://example.invalid',403,'forbidden',{},None)))
    def test_seal_retry_backoff_is_thirty_seconds(self):
        calls=[]
        def once(*a):
            calls.append('check')
            if len(calls)==1:raise runtime.ssl.SSLError('transient read')
        with patch.object(runtime,'_verify_ci_once',side_effect=once),patch.object(runtime.time,'sleep') as slept,patch.object(runtime.time,'time',return_value=supervisor.epoch(9)):
            runtime.verify_ci_for_seal(Path('no-native'),configuration())
        slept.assert_called_once_with(30)


if __name__ == "__main__": unittest.main(verbosity=2)
