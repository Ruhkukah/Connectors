"""Portable control-plane regressions; all external/runtime APIs are mocked."""
import copy
import datetime as dt
import importlib.util
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
    for name, hm in {
        "scheduled_start_utc": (9, 0), "product_start_latest_utc": (10, 10),
        "admission_latest_utc": (10, 15), "earliest_entry_utc": (10, 0),
        "hard_entry_deadline_utc": (10, 45), "flat_deadline_utc": (10, 50),
        "guardian_trigger_utc": (10, 55), "hard_cleanup_deadline_utc": (11, 0),
    }.items():
        cfg[name] = supervisor.epoch(*hm)
    return cfg


class ScopeTests(unittest.TestCase):
    def test_only_exact_owner_scope_and_fixed_risk_pass(self):
        supervisor.validate(configuration())
        changes = [("authorized_date", "2026-10-19"), ("owner_authorized", False),
                   ("timer_paused", False), ("no_watchdog_sigkill", False),
                   ("source_git_sha", "c" * 40), ("binary_sha256", "d" * 64),
                   ("coverage_scope", "FULL_DAY"), ("closeout_mode", "open_session_sigterm"),
                   ("latency_gating", True), ("partial_fill_enabled", True),
                   ("predecessor_clean_terminal_verified", False), ("environment", "prod"),
                   ("quantity_limit", 10), ("position_limit", 10),
                   ("open_order_limit", 6), ("rate_limit", 6)]
        for key, value in changes:
            with self.subTest(key=key), self.assertRaises(Refused):
                supervisor.validate(dict(configuration(), **{key: value}))

    def test_every_sealed_start_entry_fault_and_cleanup_boundary_is_exact(self):
        for key in ("scheduled_start_utc", "product_start_latest_utc", "admission_latest_utc",
                    "earliest_entry_utc", "hard_entry_deadline_utc", "flat_deadline_utc",
                    "guardian_trigger_utc", "hard_cleanup_deadline_utc"):
            cfg = configuration(); cfg[key] += 1
            with self.subTest(key=key), self.assertRaisesRegex(Refused, "sealed_window"):
                supervisor.validate(cfg)

    def test_no_product_risk_authority_before_friday_1000(self):
        runner = supervisor.Supervisor(); runner.cfg = configuration(); runner.events = []
        with patch.object(supervisor.time, "time", return_value=supervisor.epoch(9, 59)):
            with self.assertRaisesRegex(Refused, "no_entry_before"):
                runner.risk_gate(object())
        self.assertEqual(runner.events, [])
        product = object()
        with patch.object(supervisor.time, "time", return_value=supervisor.epoch(10)):
            self.assertIs(runner.risk_gate(product), product)
        self.assertEqual(runner.events, ["native_risk_gate"])

    def test_two_start_headroom_does_not_weaken_native_7000_gate(self):
        runner = supervisor.Supervisor(); runner.product = object(); runner.meta = {}
        for budget in (6999, 7000, 7999):
            runner.rt = types.SimpleNamespace(budget=lambda _, n=budget: n)
            with self.subTest(budget=budget), self.assertRaisesRegex(Refused, "headroom"):
                runner.start()
        runner.rt = types.SimpleNamespace(budget=lambda _: 8000)
        self.assertIs(runner.start(), runner.product)


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
            "commits/" + review + "/check-runs": dict(check_runs=[dict(name=name, status="completed", conclusion="success") for name in sorted(runtime.JOBS)]),
        }
        def fetch(suffix): self.calls.append(suffix); return self.data[suffix]
        self.fetch = patch.object(runtime, "fetch", side_effect=fetch); self.fetch.start()
        self.atomic = patch.object(runtime_base, "atomic", side_effect=lambda path, data:self.writes.append((path, data))); self.atomic.start()
    def tearDown(self): self.atomic.stop(); self.fetch.stop()
    def comparison(self): return self.data["compare/" + runtime.HEAD + "..." + self.cfg["review_git_sha"]]
    def test_review_head_may_differ_from_native_but_both_exact_ci_pins_pass(self):
        runtime.verify_ci(Path("test-root"), self.cfg)
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
                runtime.verify_ci(Path("test-root"),self.cfg)
        self.assertEqual(self.writes,[])
    def test_comparison_must_be_complete_ancestor_and_bounded(self):
        original=copy.deepcopy(self.comparison())
        for change in (dict(merge_base_commit=dict(sha="e"*40)),dict(status="diverged"),dict(total_commits=251),dict(total_commits=True),
                       dict(files=[dict(filename="CERTIFICATION.md",status="modified")]*300)):
            current=self.comparison();current.clear();current.update(copy.deepcopy(original));current.update(change)
            with self.subTest(change=list(change)),self.assertRaisesRegex(Refused,"comparison_incomplete"):
                runtime.verify_ci(Path("test-root"),self.cfg)
        self.assertEqual(self.writes,[])
    def test_native_pin_and_review_run_cannot_be_substituted(self):
        for change in (dict(ci_run_id=123),dict(source_git_sha="f"*40),dict(review_git_sha="short")):
            with self.subTest(change=change),self.assertRaises(Refused):
                runtime.verify_ci(Path("test-root"),dict(self.cfg,**change))
        self.data["actions/runs/123"]["head_sha"]=runtime.HEAD
        with self.assertRaisesRegex(Refused,"review_exact_ci"):
            runtime.verify_ci(Path("test-root"),self.cfg)
        self.assertEqual(self.writes,[])
    def test_each_exact_run_and_both_latest_jobs_must_be_green(self):
        for suffix in ("actions/runs/"+str(runtime.CI_RUN_ID),"actions/runs/123"):
            self.data[suffix]["conclusion"]="failure"
            with self.subTest(suffix=suffix),self.assertRaisesRegex(Refused,"exact_ci"):
                runtime.verify_ci(Path("test-root"),self.cfg)
            self.data[suffix]["conclusion"]="success"
        checks=self.data["commits/"+self.cfg["review_git_sha"]+"/check-runs"]["check_runs"]
        checks[0]["status"]="in_progress"
        with self.assertRaisesRegex(Refused,"both_ci_jobs"):
            runtime.verify_ci(Path("test-root"),self.cfg)
        checks.pop(0)
        with self.assertRaisesRegex(Refused,"both_ci_jobs"):
            runtime.verify_ci(Path("test-root"),self.cfg)
        self.assertEqual(self.writes,[])
    def test_pr_must_remain_open_draft_at_exact_review_head(self):
        for change in (dict(state="closed"),dict(draft=False),dict(head=dict(sha=runtime.HEAD))):
            original=copy.deepcopy(self.data["pulls/68"]);self.data["pulls/68"].update(change)
            with self.subTest(change=change),self.assertRaisesRegex(Refused,"review_head"):
                runtime.verify_ci(Path("test-root"),self.cfg)
            self.data["pulls/68"]=original
        self.assertEqual(self.writes,[])


class CleanupOrderingTests(unittest.TestCase):
    def runner(self, pending=True):
        runner=supervisor.Supervisor();runner.events=[];runner.public=[]
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
        self.assertEqual(runner.events,["restore_wait","close_pipe","unlock","base_cleanup"])
        self.assertFalse(hasattr(runner,"pending_fault_controller"))
    def test_uncertain_restore_retains_lock_and_does_not_trade_or_cleanup(self):
        for raises in (False,True):
            runner=self.runner()
            def pending():
                runner.events.append("restore_wait")
                if raises:raise RuntimeError("restoration not verified")
            runner.pending_fault_controller.wait_fault_worker=pending
            with patch.object(supervisor.os,"close") as close:
                with self.subTest(raises=raises),self.assertRaises(Exception):runner.fail(RuntimeError("initiating"))
            close.assert_not_called()
            self.assertEqual(runner.events,["restore_wait"])
            self.assertEqual(runner.public[-1]["flat"],"UNKNOWN")
            self.assertIs(runner.public[-1]["terminal"],False)
            self.assertTrue(hasattr(runner,"pending_fault_controller"))
    def test_no_pending_fault_uses_existing_owned_cleanup(self):
        runner=self.runner(False)
        self.assertEqual(runner.fail(RuntimeError()),"cleanup_result")
        self.assertEqual(runner.events,["base_cleanup"])


if __name__ == "__main__": unittest.main(verbosity=2)
