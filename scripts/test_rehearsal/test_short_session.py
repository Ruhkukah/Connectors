"""Offline short-session regressions; no process, socket, signal or native API."""
import copy
import datetime as dt
import importlib.util
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

# The protected supervisor supplies the frozen day_driver module at runtime.
# Unit tests provide only that public API; no private source or native code loads.
base = types.ModuleType("day_driver")
base.MSK = dt.timezone(dt.timedelta(hours=3))
base.DayStop = type("DayStop", (RuntimeError,), {})
base.TERMINAL = {"Filled", "Cancelled", "Rejected"}
base.ops = types.SimpleNamespace(ready=lambda *_args, **_kwargs: False, flat=lambda _: False)
base.price_caps = types.SimpleNamespace(current_reference=lambda *_args, **_kwargs: None)
base.sequence = types.SimpleNamespace(_price=lambda units: str(units / 100000))


class MockDayDriver:
    """Only the inherited API called by the actual short-session adapter."""
    def __init__(self, product, cfg, work, env, publish, *, wall=None, monotonic=None, sleep=None):
        self.product, self.cfg, self.work, self.env, self.publish = product, cfg, Path(work), env, publish
        self.wall, self.monotonic, self.sleep = wall, monotonic, sleep
        self.targets = product.targets
        self.timeline = Timeline()
        product.status()
    def product_entry_ready(self, state):
        if state.get("sess_id") != self.schedule["sess_id"] or not base.ops.ready(state, self.targets):
            return False
        try:
            for target in self.targets:
                base.price_caps.current_reference(state, target, now_ns=int(self.wall() * 1e9))
        except ValueError:
            return False
        return True
    def burst(self):
        raise AssertionError("replace inherited API with explicit test result")
    def restart_working(self):
        raise AssertionError("native restart must not execute in unit tests")
    def closeout(self):
        raise AssertionError("native closeout must not execute in unit tests")


base.DayDriver = MockDayDriver
spec = importlib.util.spec_from_file_location("_short_session_under_test", Path(__file__).with_name("short_session.py"))
h = importlib.util.module_from_spec(spec)
with patch.dict(sys.modules, {"day_driver": base}):
    spec.loader.exec_module(h)


def row(event, data, at):
    return dict(begin=at, end=at + 1, event=dict(event=event, data=data))


def replies():
    out = []
    for index in range(30):
        add = index % 2 == 0
        out += [row("command", dict(name="AddOrder" if add else "DelOrder", user_id=index+1), index*3),
                row("reply", dict(user_id=index+1, msgid=179 if add else 177, code=0), index*3+1),
                row("transaction_commit", dict(stream="FORTS_TRADE_REPL"), index*3+2)]
    return out


def proof():
    return dict(status="PASS", working_order_preserved=True,
                local_refusal=dict(local_refusal=True, native_posts=0, identifier_highwaters_unchanged=True, probe_attempts=1),
                native_witness=dict(observed_native_outage=True, all_streams_current_native_ready=True, fresh_trade_commits=1))


class Timeline:
    def __init__(self):
        self.cursor = types.SimpleNamespace(offset=0)
        self.session_states, self.instrument_states = {}, {}
        self.instrument_session = None
        self.restart_windows = []
        self.rows = []
    def update(self): pass
    def between(self, _): return self.rows


class Seq:
    def __init__(self):
        self.order_row = dict(client_order_id="owned", instance_owned=True, order_id=80, ext_id=10,
                              isin_id=1, quantity=1, remaining=1, executed=0, price="1", state="Working")
        self.active = False
        self.sent = []
        self.cancelled = []
    def snapshot(self):
        return dict(complete=True, entry=True, queued=0, positions=[dict(xpos=0)],
                    orders=[self.order_row] if self.active else [])
    def passive(self, *_): return 100000
    def next_id(self, _): return "owned"
    def send(self, line): self.sent.append(line); self.active = True
    def accepted_order(self, *_): return self.order_row
    def order(self, *_): return self.order_row
    def cancel(self, client):
        self.cancelled.append(client)
        self.order_row.update(state="Cancelled", remaining=0)


class ShortTests(unittest.TestCase):
    def setUp(self):
        self.now = h.epoch(10)
        self.d = h.ShortDriver.__new__(h.ShortDriver)
        self.d.wall = lambda: self.now
        self.d.sleep = lambda seconds: setattr(self, "now", self.now + seconds)
        self.d.monotonic = self.d.wall
        self.d.timeline = Timeline()
        self.d.targets = [dict(isin_id=1, symbol="ALRS-12.26"), dict(isin_id=2, symbol="CNY-12.26")]
        self.d.schedule = dict(sess_id=42, cleanup_at=h.epoch(10,45))
        self.d.env, self.d.cfg = {}, dict(authorized_date=h.DATE)
        self.d.product = object()
        self.d.commands = []
        self.d.command = self.d.commands.append
        self.ready = patch.object(base.ops, "ready", side_effect=lambda s, _, entry=True: s.get("complete") is True and (not entry or s.get("entry") is True))
        self.ready.start()
        self.flat = patch.object(base.ops, "flat", side_effect=lambda s: s.get("queued", 0) == 0 and all(p["xpos"] == 0 for p in s.get("positions", [])) and all(o["state"] in base.TERMINAL for o in s.get("orders", [])))
        self.flat.start()
        def reference(s, *_args, **_kwargs):
            if s.get("books") != "fresh": raise ValueError("current_price_two_sided_book_unproved")
            return 100000
        self.prices = patch.object(base.price_caps, "current_reference", side_effect=reference)
        self.prices.start()
    def tearDown(self):
        self.prices.stop(); self.flat.stop(); self.ready.stop()
    def state(self, **changes):
        return dict(dict(sess_id=42, complete=True, entry=True, books="fresh", positions=[], orders=[], queued=0), **changes)

    def test_friday_monday_trade_date_does_not_use_calendar_schedule_filter(self):
        state=self.state(trading_date="2026-10-19")
        self.d.status=lambda: state
        self.assertEqual(self.d.admission()["sess_id"],42)

    def test_admission_one_product_snapshot_per_iteration_and_waits_for_fresh_books(self):
        states=iter([self.state(books="one-sided"),self.state(books="stale"),self.state()]); calls=[]
        self.d.status=lambda: calls.append(1) or next(states)
        self.assertEqual(self.d.admission()["status"],"PASS")
        self.assertEqual(len(calls),3); self.assertEqual(self.d.commands,[])

    def test_journal_cannot_grant_false_product_readiness(self):
        self.d.timeline.session_states[42]=dict(fields=dict(public_state=1))
        self.assertFalse(self.d.product_entry_ready(self.state(entry=False)))

    def test_known_native_auction_vetoes_otherwise_ready_product(self):
        self.d.timeline.session_states[42]=dict(fields=dict(public_state=2))
        self.assertFalse(self.d.product_entry_ready(self.state()))
        self.d.timeline.session_states.clear()
        self.d.timeline.instrument_session=42
        self.d.timeline.instrument_states[1]=dict(fields=dict(public_state=2))
        self.assertFalse(self.d.product_entry_ready(self.state()))

    def test_absent_journal_authority_does_not_block_true_product_status(self):
        self.assertTrue(self.d.product_entry_ready(self.state()))

    def test_no_entry_before_1000_or_at_1045_or_wrong_session(self):
        for timestamp, state in ((h.epoch(9,59),self.state()),(h.epoch(10,45),self.state()),(h.epoch(10),self.state(sess_id=99))):
            self.now=timestamp
            self.assertFalse(self.d.product_entry_ready(state))

    def test_no_product_before_0900(self):
        self.now=h.epoch(8,59)
        self.d.status=lambda:self.fail("status should not be called")
        with self.assertRaisesRegex(base.DayStop,"before_0900"): self.d.admission()

    def test_admission_deadline_stops_without_order(self):
        self.now=h.epoch(10,15)
        self.d.status=lambda:self.fail("deadline already expired")
        with self.assertRaisesRegex(base.DayStop,"admission_deadline"): self.d.admission()
        self.assertEqual(self.d.commands,[])

    def test_restart_wait_entry_one_snapshot_per_check_and_no_dispatch(self):
        states=iter([self.state(books="one-sided"),self.state()]);calls=[]
        self.d.status=lambda:calls.append(1) or next(states)
        self.d.current_phase=lambda:"main"
        result=self.d.wait_entry(h.epoch(10,15),require_continuous=True)
        self.assertEqual(result["books"],"fresh")
        self.assertEqual(calls,[1,1]);self.assertEqual(self.d.commands,[])

    def test_restart_wait_can_preserve_working_orders_without_claiming_flat(self):
        state=self.state(orders=[dict(state="Working")])
        self.d.status=lambda:state
        self.assertIs(self.d.wait_entry(h.epoch(10,15),require_flat=False),state)
        with self.assertRaisesRegex(base.DayStop,"entry_not_flat"):
            self.d.wait_entry(h.epoch(10,15))

    def test_restart_wait_rechecks_deadline_after_slow_snapshot(self):
        def status():self.now=h.epoch(10,15);return self.state()
        self.d.status=status
        with self.assertRaisesRegex(base.DayStop,"readiness_deadline"):
            self.d.wait_entry(h.epoch(10,15))

    def setup_fault(self, callback=None):
        seq=Seq(); self.d.seq=lambda _:seq
        self.d.wait_passive_price=lambda _seq,state,select,**_: (state,select(state))
        self.d.env["faultcase"]=callback or (lambda *_:proof())
        return seq

    def test_fault_preserves_then_cancels_one_owned_order_and_flat(self):
        seq=self.setup_fault()
        result=self.d.fault("router","router")
        self.assertTrue(result["flat_after_case"])
        self.assertEqual(seq.cancelled,["owned"])
        self.assertEqual(len(seq.sent),1)
        self.assertEqual(self.d.commands,["kill off","kill on"])
        self.assertEqual(self.d.timeline.restart_windows,[[self.now,self.now]])

    def test_each_fault_gets_fresh_order(self):
        first=self.setup_fault(); self.d.fault("router","router")
        second=self.setup_fault(); self.d.fault("upstream","upstream")
        self.assertIsNot(first.order_row,second.order_row)
        self.assertEqual(len(first.sent)+len(second.sent),2)

    def test_fault_uncertainty_never_retries_add_and_closes_expected_window(self):
        def error(*_): raise RuntimeError("uncertain")
        seq=self.setup_fault(error)
        with self.assertRaisesRegex(RuntimeError,"uncertain"):self.d.fault("router","router")
        self.assertEqual(len(seq.sent),1)
        self.assertNotEqual(self.d.timeline.restart_windows[0][1],float("inf"))

    def test_fault_requires_single_refusal_and_no_posts(self):
        for change in (dict(native_posts=1),dict(probe_attempts=2),dict(identifier_highwaters_unchanged=False)):
            p=proof();p["local_refusal"].update(change);seq=self.setup_fault(lambda *_:p)
            with self.assertRaisesRegex(base.DayStop,"refusal_and_recovery"):self.d.fault("router","router")
            self.assertEqual(len(seq.sent),1)

    def test_fault_refuses_changed_order_identity_fill_or_foreign_owner(self):
        for change in (dict(order_id=81),dict(executed=1),dict(instance_owned=False)):
            seq=self.setup_fault()
            self.d.env["faultcase"]=lambda *_, q=seq,c=change: (q.order_row.update(c) or proof())
            with self.assertRaisesRegex(base.DayStop,"owned_order_or_position_changed"):self.d.fault("router","router")

    def test_no_fault_if_recovery_cleanup_budget_missing(self):
        self.now=h.epoch(10,43)
        self.setup_fault()
        with self.assertRaisesRegex(base.DayStop,"budget_insufficient"):self.d.fault("router","router")
        self.assertEqual(self.d.commands,[])

    def test_burst_all_thirty_replies_and_commits_match(self):
        self.assertEqual(h.burst_reply_proof(replies())["matching_code0_replies"],30)

    def test_burst_rejects_missing_duplicate_wrong_or_nonzero_reply(self):
        for mutation in (lambda r:r.pop(1),lambda r:r.append(copy.deepcopy(r[1])),lambda r:r[1]["event"]["data"].update(msgid=177),lambda r:r[1]["event"]["data"].update(code=14),lambda r:r[1]["event"]["data"].update(code=False)):
            rows=replies();mutation(rows)
            with self.assertRaisesRegex(base.DayStop,"matching_code0_reply"):h.burst_reply_proof(rows)

    def test_burst_nonzero_reply_is_fatal_without_wait_or_command_retry(self):
        self.d.status=lambda:self.state()
        self.d.timeline.rows=replies()
        self.d.timeline.rows[1]["event"]["data"]["code"]=14
        self.d.sleep=lambda _:self.fail("contradictory reply must fail immediately")
        with patch.object(base.DayDriver,"burst",return_value=dict(status="PASS",native_posts=30)):
            with self.assertRaisesRegex(base.DayStop,"reply_unproved"):self.d.burst()

    def test_burst_rejects_duplicate_user_and_missing_last_commit(self):
        rows=replies();rows[3]["event"]["data"]["user_id"]=1
        with self.assertRaisesRegex(base.DayStop,"unique_Add_Del_posts"):h.burst_reply_proof(rows)
        rows=replies();rows.pop()
        with self.assertRaisesRegex(base.DayStop,"later_trade_commit"):h.burst_reply_proof(rows)

    def test_burst_calls_inherited_thirty_post_limiter_then_checks_replies(self):
        self.d.status=lambda:self.state()
        self.d.timeline.rows=replies()
        with patch.object(base.DayDriver,"burst",return_value=dict(status="PASS",native_posts=30,max_posts_per_rolling_second=5)) as inherited:
            result=self.d.burst()
        inherited.assert_called_once_with();self.assertEqual(result["matching_code0_replies"],30)

    def test_pre_SIGTERM_requires_real_cleanup_and_current_complete_flat(self):
        self.d.env["flatten"]=lambda *_:dict(cancel_confirmed=True,flat=True)
        self.d.status=lambda:self.state()
        self.assertEqual(self.d.prepare_empty_closeout()["status"],"PASS")
        self.assertEqual(self.d.commands,["kill on"])
        self.d.status=lambda:self.state(complete=False)
        with self.assertRaisesRegex(base.DayStop,"complete_flatness"):self.d.prepare_empty_closeout()

    def test_flat_deadline_refuses_without_cleanup(self):
        self.now=h.epoch(10,50)
        self.d.env["flatten"]=lambda *_:self.fail("cleanup deadline expired")
        with self.assertRaisesRegex(base.DayStop,"flat_deadline_passed"):self.d.prepare_empty_closeout()

    def test_constructor_uses_supplied_base_and_one_initial_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory,"interaction.ndjson").touch()
            calls=[]
            product=types.SimpleNamespace(targets=self.d.targets,status=lambda:calls.append(1) or self.state())
            driver=h.ShortDriver(product,dict(authorized_date=h.DATE,closeout_mode="flat_then_observe_clearing"),directory,{},lambda _:None,wall=lambda:self.now)
            self.assertIsInstance(driver,base.DayDriver)
            self.assertEqual(calls,[1])
            self.assertEqual(driver.schedule["cleanup_at"],h.epoch(10,45))
            self.assertTrue(driver.gate_passed)

    def test_run_only_requested_short_scenarios_no_full_day_partial_or_gate(self):
        stages=[]
        self.d.scenarios={};self.d.restart_used=True
        self.d.stage=lambda name,callback:stages.append(name)
        self.d.timeline.path=Path(__file__)
        self.d.timeline.kind_bytes={};self.d.timeline.kind_counts={};self.d.timeline.timing=lambda _:{}
        result=self.d.run()
        self.assertEqual(stages,["short_admission","planned_restart_two_working","one_passive_router_restart","one_passive_upstream_socket_loss","rate_burst_thirty","pre_SIGTERM_cancel_flat","terminal_closeout"])
        self.assertEqual(result["coverage_scope"],"SHORT_INTERRUPTION_ONLY")


if __name__ == "__main__": unittest.main(verbosity=2)
