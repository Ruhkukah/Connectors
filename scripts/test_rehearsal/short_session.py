"""Sealed short TEST-session adapter; importing performs no native action."""
from __future__ import annotations

import datetime as dt

import day_driver as base

DATE = "2026-10-16"
DayStop = base.DayStop
ops, sequence, price_caps = base.ops, base.sequence, base.price_caps


def epoch(hour, minute=0):
    return dt.datetime(2026, 10, 16, hour, minute, tzinfo=base.MSK).timestamp()


def burst_reply_proof(rows):
    """Require each actual Add/Del post's unique code-0 reply and later commit."""
    commands = [r for r in rows if r["event"]["event"] == "command"]
    names = [r["event"]["data"].get("name") for r in commands]
    users = [r["event"]["data"].get("user_id") for r in commands]
    if (len(commands) != 30 or names.count("AddOrder") != 15 or names.count("DelOrder") != 15
            or any(type(x) is not int or x <= 0 for x in users) or len(set(users)) != 30):
        raise base.DayStop("short_burst_thirty_unique_Add_Del_posts_unproved")
    evidence = []
    for command in commands:
        data = command["event"]["data"]
        replies = [r for r in rows if r["event"]["event"] == "reply"
                   and r["event"]["data"].get("user_id") == data["user_id"]]
        expected = 179 if data["name"] == "AddOrder" else 177
        if not replies:
            raise base.DayStop("short_burst_matching_code0_reply_pending")
        if (len(replies) != 1 or replies[0]["begin"] < command["end"]
                or replies[0]["event"]["data"].get("msgid") != expected
                or type(replies[0]["event"]["data"].get("code")) is not int
                or replies[0]["event"]["data"]["code"] != 0):
            raise base.DayStop("short_burst_matching_code0_reply_unproved")
        reply = replies[0]
        commit = next((r for r in rows if r["begin"] >= reply["end"]
                       and r["event"]["event"] == "transaction_commit"
                       and r["event"]["data"].get("stream") == "FORTS_TRADE_REPL"), None)
        if commit is None:
            raise base.DayStop("short_burst_later_trade_commit_unproved")
        evidence.append(dict(user_id=data["user_id"], command_offset=command["begin"],
                             reply_offset=reply["begin"], commit_offset=commit["end"]))
    return dict(matching_code0_replies=30, later_trade_commits_verified=True,
                command_reply_commit_offsets=evidence)


class ShortDriver(base.DayDriver):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        if self.cfg.get("authorized_date") != DATE or self.cfg.get("closeout_mode") != "flat_then_observe_clearing":
            raise base.DayStop("short_date_or_empty_closeout_mode_unproved")
        self.schedule = dict(sess_id=None, cleanup_at=epoch(10, 45), stop_entry_at=epoch(10, 45),
                             flat_deadline=epoch(10, 50), last_end=epoch(11),
                             gate_deadline=epoch(10, 15), intervals=[dict(name="main", begin=epoch(10), end=epoch(11))])
        self.gate_passed = True  # No opening/latency gate belongs to this short session.

    def product_entry_ready(self, state):
        if self.wall() < epoch(10) or self.wall() >= epoch(10, 45):
            return False
        if not super().product_entry_ready(state):
            return False
        # Native journal state can veto a known auction/closed state. Missing
        # journal rows never grant entry; product status and fresh books do.
        session = self.timeline.session_states.get(state.get("sess_id"))
        if session and session["fields"].get("public_state") != 1:
            return False
        if self.timeline.instrument_session == state.get("sess_id"):
            for target in self.targets:
                row = self.timeline.instrument_states.get(target["isin_id"])
                if row and row["fields"].get("public_state") != 1:
                    return False
        return True

    def admission(self):
        if self.wall() < epoch(9):
            raise base.DayStop("short_product_started_before_0900")
        while self.wall() < epoch(10, 15):
            state = self.status()  # One status snapshot per admission iteration.
            if type(state.get("sess_id")) is int and state["sess_id"] > 0:
                self.schedule["sess_id"] = state["sess_id"]
            if self.product_entry_ready(state):
                if not base.ops.flat(state):
                    raise base.DayStop("short_admission_not_flat")
                return dict(status="PASS", sess_id=state["sess_id"],
                            admitted_utc=self.wall(), authority="product_status_and_fresh_two_sided_books")
            self.sleep(.2 if self.wall() >= epoch(10) else min(5, epoch(10) - self.wall()))
        raise base.DayStop("short_admission_deadline")

    def case_budget(self, seconds):
        if self.wall() + seconds >= epoch(10, 45):
            raise base.DayStop("short_remaining_entry_fault_cleanup_budget_insufficient")

    def wait_entry(self, deadline, require_flat=True, require_continuous=False):
        """Read-only readiness wait using one authoritative status per check.

        Every later command still passes the Product's separate pre-intent risk
        gate. A transient snapshot here never becomes a retried command.
        """
        deadline = min(deadline, epoch(10, 45))
        while self.wall() < deadline:
            state = self.status()
            if self.product_entry_ready(state) and (not require_continuous or self.current_phase() is not None):
                if require_flat and not base.ops.flat(state):
                    raise base.DayStop("entry_not_flat")
                if self.wall() >= deadline:
                    raise base.DayStop("native_entry_readiness_deadline")
                return state
            self.sleep(.2)
        raise base.DayStop("native_entry_readiness_deadline")

    def fault(self, name, kind):
        self.case_budget(180)
        seq = self.seq(name)
        state = seq.snapshot()
        if not base.ops.ready(state, self.targets, entry=False) or not base.ops.flat(state):
            raise base.DayStop("short_fault_initial_complete_flatness_unproved")
        self.command("kill off")  # The existing Product risk guard rechecks here.
        target = self.targets[0]
        state, price = self.wait_passive_price(seq, state, lambda st: seq.passive(st, target, 1),
                                               deadline=min(epoch(10, 45) - 180, self.wall() + 120))
        client = seq.next_id(target)
        seq.send(f"place {client} {target['isin_id']} buy 1 {base.sequence._price(price)} day")
        before = seq.accepted_order(target, client)
        if (before["state"] != "Working" or before["executed"] != 0 or before["remaining"] != 1
                or before.get("instance_owned") is not True or before.get("quantity") != 1):
            raise base.DayStop("short_single_passive_identity_unproved")
        original = {key: before[key] for key in ("order_id", "ext_id", "isin_id", "quantity", "remaining", "executed", "price")}
        self.command("kill on")
        window = [self.wall(), float("inf")]
        self.timeline.restart_windows.append(window)
        try:
            proof = self.env["faultcase"](self.product, name, kind, seq, client, target)
            local = proof.get("local_refusal", {})
            witness = proof.get("native_witness", {})
            if (proof.get("status") != "PASS" or proof.get("working_order_preserved") is not True
                    or local.get("local_refusal") is not True or local.get("native_posts") != 0
                    or local.get("identifier_highwaters_unchanged") is not True or local.get("probe_attempts") != 1
                    or witness.get("observed_native_outage") is not True
                    or witness.get("all_streams_current_native_ready") is not True
                    or type(witness.get("fresh_trade_commits")) is not int or witness["fresh_trade_commits"] < 1):
                raise base.DayStop("short_fault_refusal_and_recovery_proof_unproved")
            state = seq.snapshot()
            after = seq.order(state, client)
            if (not base.ops.ready(state, self.targets, entry=False) or after is None
                    or after.get("instance_owned") is not True or after["state"] != "Working"
                    or {key: after[key] for key in original} != original
                    or any(p["xpos"] for p in state["positions"]) or state["queued"] != 0
                    or len([o for o in state["orders"] if o["state"] not in base.TERMINAL]) != 1):
                raise base.DayStop("short_fault_owned_order_or_position_changed")
            seq.cancel(client)
            state = seq.snapshot()
            if not base.ops.ready(state, self.targets, entry=False) or not base.ops.flat(state):
                raise base.DayStop("short_fault_cancelled_complete_flatness_unproved")
        finally:
            window[1] = self.wall()
            self.timeline.update()
        return dict(status="PASS", working_order_preserved=True, cancelled_after_recovery=True,
                    flat_after_case=True, fault_proof=proof)

    def burst(self):
        self.case_budget(120)
        self.command("kill off")
        self.timeline.update()
        begin = self.timeline.cursor.offset
        result = super().burst()
        deadline = min(epoch(10, 45), self.wall() + 10)
        while True:
            self.timeline.update()
            try:
                proof = burst_reply_proof(self.timeline.between(begin))
                break
            except base.DayStop as error:
                if str(error) not in ("short_burst_matching_code0_reply_pending", "short_burst_later_trade_commit_unproved") or self.wall() >= deadline:
                    raise
                self.status()
                self.sleep(.05)  # Read-only journal catch-up; no command is retried.
        state = self.status()
        if not base.ops.ready(state, self.targets, entry=False) or not base.ops.flat(state):
            raise base.DayStop("short_burst_final_complete_flatness_unproved")
        return dict(result, **proof)

    def prepare_empty_closeout(self):
        self.closing = True
        self.closing_phase = "final_cleanup"
        self.command("kill on")
        if self.wall() >= epoch(10, 50):
            raise base.DayStop("short_flat_deadline_passed")
        result = self.env["flatten"](self.product, epoch(10, 50))
        state = self.status()
        if (result.get("cancel_confirmed") is not True or result.get("flat") is not True
                or self.wall() > epoch(10, 50) or not base.ops.ready(state, self.targets, entry=False)
                or not base.ops.flat(state)):
            raise base.DayStop("short_pre_SIGTERM_complete_flatness_unproved")
        return dict(status="PASS", cancel_confirmed=True, flat=True)

    def run(self):
        self.stage("short_admission", self.admission)
        self.case_budget(900)
        self.stage("planned_restart_two_working", self.restart_working)
        for name, kind in (("one_passive_router_restart", "router"), ("one_passive_upstream_socket_loss", "upstream")):
            self.stage(name, lambda n=name, k=kind: self.fault(n, k))
        self.stage("rate_burst_thirty", self.burst)
        self.stage("pre_SIGTERM_cancel_flat", self.prepare_empty_closeout)
        self.stage("terminal_closeout", self.closeout)
        return dict(terminal=True, execution_result="PASS", coverage_scope="SHORT_INTERRUPTION_ONLY",
                    scenarios=self.scenarios, planned_restarts=int(self.restart_used),
                    journal_bytes=self.timeline.path.stat().st_size,
                    journal_bytes_by_kind=self.timeline.kind_bytes,
                    journal_records_by_kind=self.timeline.kind_counts,
                    native_timing=self.timeline.timing(0), timer_paused=True)


def run(product, cfg, work, env, publish):
    return ShortDriver(product, cfg, work, env, publish).run()


if __name__ == "__main__":
    raise SystemExit("Protected supervisor import only; standalone execution refused.")
