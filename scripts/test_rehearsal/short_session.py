"""Sealed short TEST-session adapter; importing performs no native action."""
from __future__ import annotations

import datetime as dt

import day_driver as base
import short_scope as scope
DayStop = base.DayStop
ops, sequence, price_caps = base.ops, base.sequence, base.price_caps


TRANSIENT_PRICE_REASONS = frozenset(("current_price_book_unproved", "current_price_book_stale",
    "current_price_two_sided_book_unproved", "passive_quote_unavailable",
    "passive_price_not_far_enough", "mass_three_lower_band_not_safely_passive", "instrument_not_ready"))


class PreIntentNotSent(base.DayStop):
    not_sent = True


class RateCheckFailed(base.DayStop):
    rate_only = True

    def __init__(self, metrics):
        self.metrics = metrics
        super().__init__("short_burst_native_five_per_second_limit_exceeded")


def burst_rate_metrics(posts):
    """Report strict wall-clock density separately from native-ms-compatible density.

    Native admission truncates steady time to ms. Allowing exactly 1 ms at
    the measurement boundary avoids treating sub-ms quantization as a new
    connector regression; the strict ns count is still reported unchanged.
    """
    def maximum(window):
        left = result = 0
        for right, stamp in enumerate(posts):
            while stamp - posts[left] >= window:
                left += 1
            result = max(result, right - left + 1)
        return result
    return dict(max_posts_per_rolling_second=maximum(999_000_000),
                max_posts_per_strict_ns_second=maximum(1_000_000_000),
                rate_window_slack_ns=1_000_000,
                rate_semantics="native_millisecond_admission_with_1ms_measurement_slack")


def inventory_signature(state):
    """Complete known inventory only; queued commands are never treated flat."""
    if type(state.get("queued")) is not int or state["queued"] != 0:
        raise base.DayStop("passive_wait_queued_command_unresolved")
    owned = []
    for row in state.get("orders", []):
        if row.get("instance_owned") is not True or row.get("operator_action_required", False):
            raise base.DayStop("passive_wait_order_ownership_unproved")
        if row.get("state") not in base.TERMINAL:
            owned.append(tuple(str(row.get(key)) for key in ("client_order_id", "order_id", "ext_id", "isin_id", "quantity", "remaining", "executed", "price", "state")))
    positions = [tuple(row.get(key) for key in ("isin_id", "xpos", "buys", "sells", "last_deal_id"))
                 for row in state.get("positions", []) if any(row.get(key, 0) for key in ("xpos", "buys", "sells", "last_deal_id"))]
    return tuple(sorted(owned)), tuple(sorted(positions))


def retain_throttle_records(timeline):
    """Retain one additional native event in this adapter's timeline only."""
    cursor, events = timeline.cursor, timeline.cursor.events
    def observed_events():
        previous = cursor.offset
        for event in events():
            end = cursor.offset
            if event.get("event") == "throttle":
                timeline.records.append(dict(event=event, begin=previous, end=end,
                    stamp=dt.datetime.fromisoformat(event["utc"].replace("Z", "+00:00")).timestamp()))
            previous = end
            yield event
    cursor.events = observed_events


def burst_throttle_proof(rows, begin, end):
    """A queued Add can wait on readiness; only native rate refusal proves binding."""
    records = [row for row in rows if row["begin"] >= begin and row["end"] <= end
               and row["event"].get("event") == "throttle"
               and row["event"].get("data", {}).get("active") is True
               and type(row["event"].get("data", {}).get("queued")) is int
               and row["event"]["data"]["queued"] > 0]
    if not records:
        raise base.DayStop("short_burst_native_rate_throttle_not_observed")
    return dict(native_rate_throttle_observed=True,
                native_rate_throttle_offsets=[dict(begin=row["begin"], end=row["end"],
                    queued=row["event"]["data"]["queued"]) for row in records])


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
        try:
            self.window = scope.window(self.cfg)
        except ValueError as error:
            raise base.DayStop(str(error)) from error
        if self.cfg.get("closeout_mode") != "flat_then_observe_clearing":
            raise base.DayStop("short_empty_closeout_mode_unproved")
        self.schedule = dict(sess_id=None, cleanup_at=self.window["hard_entry_deadline_utc"],
                             stop_entry_at=self.window["hard_entry_deadline_utc"],
                             flat_deadline=self.window["flat_deadline_utc"],
                             last_end=self.window["hard_cleanup_deadline_utc"],
                             gate_deadline=self.window["admission_latest_utc"],
                             intervals=[dict(name="main", begin=self.window["earliest_entry_utc"],
                                             end=self.window["hard_entry_deadline_utc"])])
        self.gate_passed = True  # No opening/latency gate belongs to this short session.
        retain_throttle_records(self.timeline)
        self.dispatch_count = 0
        self.admission_complete = False
        self.pre_admission_window = [self.window["scheduled_start_utc"], float("inf")]
        self.timeline.restart_windows.append(self.pre_admission_window)

    def tick(self):
        if getattr(self, "admission_complete", False):
            return super().tick()
        # Before admission, a transport close is recorded, while only a fresh
        # complete native reconstruction can grant entry. Historical link/gap
        # records do not abort read-only waiting for the exchange to open.
        if self.env.get("tick"):
            self.env["tick"]()
        if self.env.get("verify_owner"):
            self.env["verify_owner"](self.product)
        if (self.work / "stop-requested").exists():
            raise base.DayStop("stop_requested")
        self.timeline.update()
        self.handled_gaps = len(getattr(self.timeline, "gaps", []))


    def status(self, entry=False):
        if not entry:
            return super().status()
        limit = min(self.wall() + 30, self.window["hard_entry_deadline_utc"])
        while self.wall() < limit:
            current = super().status()
            if self.selected_entry_ready(current, self.targets):
                return current
            self.sleep(.1)
        raise PreIntentNotSent("short_pre_intent_entry_deadline")

    def selected_entry_ready(self, state, targets):
        if (self.wall() < self.window["earliest_entry_utc"]
                or self.wall() >= self.window["hard_entry_deadline_utc"]
                or state.get("sess_id") != self.schedule["sess_id"]
                or not base.ops.ready(state, self.targets, entry=False)):
            return False
        for target in targets:
            rows = [row for row in state.get("instruments", []) if row.get("isin_id") == target["isin_id"]]
            if len(rows) != 1 or rows[0].get("symbol") != target["symbol"]:
                raise base.DayStop("short_entry_instrument_identity_unproved")
            if rows[0].get("order_entry_ready") is not True:
                return False
            try:
                base.price_caps.current_reference(state, target, now_ns=int(self.wall() * 1e9))
            except ValueError as error:
                if str(error) in TRANSIENT_PRICE_REASONS:
                    return False
                raise
        # Native journal state can veto a known auction/closed state. Missing
        # journal rows never grant entry; product status and fresh books do.
        session = self.timeline.session_states.get(state.get("sess_id"))
        if session and session["fields"].get("public_state") != 1:
            return False
        if self.timeline.instrument_session == state.get("sess_id"):
            for target in targets:
                row = self.timeline.instrument_states.get(target["isin_id"])
                if row and row["fields"].get("public_state") != 1:
                    return False
        return True

    def product_entry_ready(self, state):
        return self.selected_entry_ready(state, self.targets)

    def command(self, line):
        if line.startswith(("place ", "move ", "kill off")):
            if self.closing or self.failed:
                raise base.DayStop("new_entry_stopped_after_driver_failure_or_closeout")
            # This is a pre-intent read-only wait. Product.command performs its
            # own final owner/native guard and is invoked only once.
            targets = self.targets
            if line.startswith("place "):
                fields = line.split()
                targets = [target for target in self.targets if str(target["isin_id"]) == fields[2]]
                if len(targets) != 1:
                    raise base.DayStop("short_command_target_unproved")
            deadline = min(self.wall() + 30, self.window["hard_entry_deadline_utc"])
            while self.wall() < deadline:
                state = self.status()
                if self.selected_entry_ready(state, targets):
                    break
                self.sleep(.1)
            else:
                raise PreIntentNotSent("short_pre_intent_entry_deadline")
        return self.dispatch_once(line)

    def dispatch_once(self, line):
        """Count actual attempts; a proven pre-intent refusal adds no dispatch."""
        try:
            result = self.product.command(line)
        except Exception as error:
            if not getattr(error, "not_sent", False):
                self.dispatch_count += 1
                raise base.DayStop("short_command_dispatch_outcome_uncertain") from error
            raise
        self.dispatch_count += 1
        if not isinstance(result, dict) or result.get("ok") is not True:
            raise base.DayStop("short_command_dispatch_outcome_uncertain")
        return result

    def seq(self, stage):
        seq = base.sequence._Sequence(self.command, self.status, self.work, self.targets, stage)
        # Preserve a proven pre-intent refusal; the historical wrapper calls
        # every exception uncertain, even when no command was sent.
        def send(line):
            seq.record("command_requested", line)
            try:
                result = self.command(line)
            except Exception as error:
                if getattr(error, "not_sent", False):
                    seq.record("command_not_sent", {"exception": type(error).__name__})
                    raise
                seq.record("command_uncertain", {"exception": type(error).__name__})
                raise base.sequence.SequenceStop("command_outcome_uncertain") from error
            seq.record("command_result", result)
            if not isinstance(result, dict) or result.get("ok") is not True:
                raise base.sequence.SequenceStop("command_refused")
            self.sleep(.22)
        seq.send = send
        return seq

    def wait_passive_price(self, seq, state, select, *, deadline=None):
        limit = min(self.wall() + 30, self.window["hard_entry_deadline_utc"],
                    deadline if deadline is not None else float("inf"))
        expected = inventory_signature(state)
        while self.wall() < limit:
            if not base.ops.ready(state, self.targets, entry=False):
                raise base.DayStop("passive_wait_full_reconstruction_unproved")
            if inventory_signature(state) != expected:
                raise base.DayStop("passive_wait_owned_inventory_changed")
            try:
                return state, select(state)
            except (base.sequence.SequenceStop, base.DayStop, ValueError) as error:
                if getattr(error, "code", str(error)) not in TRANSIENT_PRICE_REASONS:
                    raise
            self.sleep(min(.1, max(0, limit - self.wall())))
            state = seq.snapshot()
        raise PreIntentNotSent("passive_price_wait_timeout")

    def admission(self):
        if self.wall() < self.window["scheduled_start_utc"]:
            raise base.DayStop("short_product_started_before_scheduled_preflight")
        while self.wall() < self.window["admission_latest_utc"]:
            state = self.status()  # One status snapshot per admission iteration.
            if type(state.get("sess_id")) is int and state["sess_id"] > 0:
                self.schedule["sess_id"] = state["sess_id"]
            if self.product_entry_ready(state):
                if not base.ops.flat(state):
                    raise base.DayStop("short_admission_not_flat")
                admitted = self.wall()
                if admitted >= self.window["admission_latest_utc"]:
                    raise base.DayStop("short_admission_deadline")
                self.pre_admission_window[1] = admitted
                self.timeline.update()
                self.handled_gaps = len(getattr(self.timeline, "gaps", []))
                self.last_complete_state = state
                self.admission_complete = True
                return dict(status="PASS", sess_id=state["sess_id"],
                            admitted_utc=admitted, authority="product_status_and_fresh_two_sided_books",
                            pre_admission_transport_events_recorded=True)
            self.sleep(.2 if self.wall() >= self.window["earliest_entry_utc"] else min(5, max(0, self.window["earliest_entry_utc"] - self.wall())))
        raise base.DayStop("short_admission_deadline")

    def case_budget(self, seconds):
        if self.wall() + seconds >= self.window["hard_entry_deadline_utc"]:
            raise base.DayStop("short_remaining_entry_fault_cleanup_budget_insufficient")

    def wait_entry(self, deadline, require_flat=True, require_continuous=False):
        """Read-only readiness wait using one authoritative status per check.

        Every later command still passes the Product's separate pre-intent risk
        gate. A transient snapshot here never becomes a retried command.
        """
        deadline = min(deadline, self.window["hard_entry_deadline_utc"])
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
        state = seq.snapshot()  # Ownership checks for kill-off may take seconds.
        state, price = self.wait_passive_price(seq, state, lambda st: seq.passive(st, target, 1),
                                               deadline=min(self.window["hard_entry_deadline_utc"] - 180, self.wall() + 30))
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

    def burst_price(self, seq, state, target, quantity):
        row = seq.target(state, target)
        terms = row["session_terms"]
        step, lower = terms["min_step_scaled"], terms["lower_price_scaled"]
        if type(step) is not int or step <= 0 or type(lower) is not int or lower <= 0:
            raise base.DayStop("short_burst_native_lower_band_unproved")
        price = ((lower + step - 1) // step) * step
        # Two q1 orders share the frozen TWO-LOWER-BAND budget. The usual
        # single-order passive price (lower+two ticks) cannot fit that budget.
        seq.valid_price(state, target, price, quantity)
        bid = base.price_caps.current_reference(state, target, now_ns=int(self.wall() * 1e9))
        distance = max(20 * step, terms["reference_price_scaled"] // 100)
        if price > bid - distance:
            raise base.sequence.SequenceStop("mass_three_lower_band_not_safely_passive")
        return price

    def burst_snapshot(self, queued_observations):
        """Queued is expected inside the bounded burst, never flatness proof."""
        deadline = min(self.wall() + 30, self.window["hard_entry_deadline_utc"])
        while True:
            if self.env.get("tick"):
                self.env["tick"]()
            self.timeline.update()
            try:
                state = self.product.status()
            except Exception as error:
                if getattr(self, "dispatch_count", 0) > getattr(self, "burst_dispatch_begin", 0):
                    raise base.DayStop("short_burst_command_confirmation_uncertain") from error
                raise
            if any(state.get(key) for key in ("journal_failed", "log_error", "cancel_only", "operator_action_required")):
                raise base.DayStop("short_burst_native_safety_alert")
            if base.ops.ready(state, self.targets, entry=False):
                break
            if self.wall() >= deadline:
                if getattr(self, "dispatch_count", 0) > getattr(self, "burst_dispatch_begin", 0):
                    raise base.DayStop("short_burst_command_confirmation_uncertain_readiness_timeout")
                raise PreIntentNotSent("short_burst_readonly_readiness_deadline_not_sent")
            self.sleep(min(.1, max(0, deadline - self.wall())))
        if (type(state.get("queued")) is not int or state["queued"] < 0
                or any(row.get("xpos") != 0 for row in state.get("positions", []))):
            raise base.DayStop("short_burst_native_queue_or_position_mismatch")
        for row in state.get("orders", []):
            if row.get("client_order_id") in getattr(self, "burst_clients", set()) and row.get("executed") != 0:
                raise base.DayStop("short_burst_owned_passive_execution_mismatch")
        open_orders = [row for row in state.get("orders", []) if row.get("state") not in base.TERMINAL]
        if len(open_orders) > 4:
            raise base.DayStop("short_burst_four_order_budget_exceeded")
        for target in self.targets:
            rows = [row for row in open_orders if row.get("isin_id") == target["isin_id"]]
            if len(rows) > 2:
                raise base.DayStop("short_burst_two_lot_instrument_budget_exceeded")
        for row in open_orders:
            if (row.get("instance_owned") is not True or row.get("operator_action_required", False)
                    or row.get("state") == "Unknown" or row.get("quantity") != 1
                    or row.get("executed") != 0 or row.get("side") != "buy"
                    or row.get("isin_id") not in {target["isin_id"] for target in self.targets}):
                raise base.DayStop("short_burst_owned_passive_identity_or_execution_mismatch")
        if state["queued"]:
            queued_observations.append(dict(queued=state["queued"], observed_utc=self.wall(),
                                            journal_offset=self.timeline.cursor.offset))
        if state["queued"] == 0:
            # Recovery signatures forbid queued commands. Refresh only a
            # current complete drained baseline, never an incomplete snapshot.
            self.last_complete_state = state
        return state

    def burst_wave_plan(self, seq, state, remaining, candidates=None):
        """Plan only a flat wave; unavailable books never increase another cap."""
        capacities, prices, blocked = {}, {}, {}
        candidates = self.targets if candidates is None else candidates
        for target in candidates:
            try:
                price = self.burst_price(seq, state, target, 1)
                if not self.selected_entry_ready(state, [target]):
                    raise ValueError("instrument_not_ready")
            except (base.sequence.SequenceStop, base.DayStop, ValueError) as error:
                reason = getattr(error, "code", str(error))
                if reason not in TRANSIENT_PRICE_REASONS:
                    raise
                blocked[target["symbol"]] = reason
                continue
            limits = next(row for row in state["configuration"]["risk"]["instruments"]
                          if row["isin_id"] == target["isin_id"])
            capacity = min(2, limits["max_position"], limits["max_quote_notional_scaled"] // price)
            seq.valid_price(state, target, price, capacity)
            capacities[target["isin_id"]] = capacity
            prices[target["isin_id"]] = price
        wave = []
        for _ in range(2):
            for target in candidates:
                isin = target["isin_id"]
                if len(wave) < remaining and capacities.get(isin, 0) > 0:
                    wave.append(target)
                    capacities[isin] -= 1
        return wave, prices, blocked

    def burst(self):
        self.case_budget(120)
        self.burst_dispatch_begin = self.dispatch_count
        self.timeline.update()
        begin = self.timeline.cursor.offset
        deadline = min(self.window["hard_entry_deadline_utc"], self.wall() + 120)
        seq = self.seq("rate-burst")
        queued, clients, waves = [], [], []
        started = self.monotonic()
        # Exactly 15 Add/Del pairs, with at most two q1 orders per eligible
        # ISIN. Each new wave starts flat; a one-sided unselected book cannot
        # block the other instrument or enlarge its frozen aggregate budget.
        self.burst_clients = set()
        enabled, completed = False, 0
        while completed < 15:
            price_deadline = min(deadline, self.wall() + 20)
            while True:
                state = self.burst_snapshot(queued)
                if not base.ops.flat(state):
                    raise base.DayStop("short_burst_wave_initial_flatness_unproved")
                targets, prices, blocked = self.burst_wave_plan(seq, state, 15 - completed)
                if self.wall() >= price_deadline:
                    error = PreIntentNotSent("short_burst_fresh_price_deadline_not_sent")
                    error.metrics = dict(completed_native_pairs=completed, wait_predicates=blocked)
                    raise error
                if not targets:
                    self.sleep(min(.05, max(0, price_deadline - self.wall())))
                    continue
                selected = [target for target in self.targets if target in targets]
                # Waiting is outside this fresh per-wave ownership lease.
                # Its preflight remains selected-instrument entry proof plus
                # complete account reconstruction/configuration/ownership.
                lease_entered = False
                dispatch_before_lease = self.dispatch_count
                try:
                    with self.env["fast_dispatch"](self.product, selected):
                        lease_entered = True
                        state = self.burst_snapshot(queued)
                        if not base.ops.flat(state):
                            raise base.DayStop("short_burst_pre_dispatch_inventory_mismatch")
                        targets, prices, blocked = self.burst_wave_plan(seq, state, 15 - completed, selected)
                        if self.wall() >= deadline:
                            raise PreIntentNotSent("short_burst_dispatch_deadline")
                        if not targets:
                            continue  # No intent; release lease before waiting/replanning.
                        if not enabled:
                            self.dispatch_once("kill off")
                            enabled = True
                        wave = [(target, seq.next_id(target)) for target in targets]
                        clients.extend(client for _, client in wave)
                        self.burst_clients.update(client for _, client in wave)
                        for target, client in wave:
                            if self.wall() >= deadline:
                                raise PreIntentNotSent("short_burst_dispatch_deadline")
                            seq.record("command_requested", dict(client_order_id=client, kind="AddOrder"))
                            self.dispatch_once(f"place {client} {target['isin_id']} buy 1 {base.sequence._price(prices[target['isin_id']])} day")
                        while True:
                            state = self.burst_snapshot(queued)
                            orders = [seq.order(state, client) for _, client in wave]
                            if any(row and row.get("state") in base.TERMINAL for row in orders):
                                raise base.DayStop("short_burst_add_terminal_before_cancel")
                            if all(row and row.get("state") == "Working" and row.get("order_id", 0) > 0 for row in orders) and state["queued"] == 0:
                                break
                            if self.wall() >= deadline:
                                raise base.DayStop("short_burst_queued_Add_confirmation_uncertain_timeout")
                            self.sleep(.02)
                        for _, client in wave:
                            # Del remains risk-reducing if the entry lease expires.
                            self.dispatch_once("cancel " + client)
                            self.burst_snapshot(queued)
                        while True:
                            state = self.burst_snapshot(queued)
                            if base.ops.flat(state):
                                break
                            if self.wall() >= deadline:
                                raise base.DayStop("short_burst_cancel_confirmation_uncertain_timeout")
                            self.sleep(.02)
                        count = len(wave)
                        waves.append(dict(adds=count, dels=count, complete_flat=True,
                                          instruments=[dict(symbol=t["symbol"], adds=targets.count(t))
                                                       for t in selected if t in targets]))
                        completed += count
                except Exception as error:
                    # Only an entry preflight that never yielded the lease
                    # and never dispatched is eligible for read-only replan.
                    # A timed-out/sent Add, owner refusal or uncertain outcome
                    # is never retried, even when another book is available.
                    if (lease_entered or self.dispatch_count != dispatch_before_lease
                            or getattr(error, "not_sent", False) is not True
                            or getattr(error, "code", str(error)) != "short_preintent_readiness_deadline_not_sent"):
                        raise
                    if self.wall() >= deadline:
                        raise PreIntentNotSent("short_burst_dispatch_deadline") from error
                    price_deadline = min(deadline, self.wall() + 20)
                    continue
                break
        while True:
            self.timeline.update()
            try:
                proof = burst_reply_proof(self.timeline.between(begin))
                break
            except base.DayStop as error:
                if str(error) not in ("short_burst_matching_code0_reply_pending", "short_burst_later_trade_commit_unproved"):
                    raise
                if self.wall() >= deadline:
                    raise base.DayStop("short_burst_native_reply_or_commit_confirmation_uncertain") from error
                self.status()
                self.sleep(.05)
        rows = self.timeline.between(begin)
        timings = [row for row in rows if row["event"]["event"] == "command_timing"]
        commands = [row for row in rows if row["event"]["event"] == "command"]
        expected_users = {row["event"]["data"]["user_id"] for row in commands}
        timing_users = [row["event"]["data"].get("user_id") for row in timings]
        posts = sorted(row["event"]["data"].get("post_started_utc_ns", 0) for row in timings)
        if (len(posts) != 30 or len(set(timing_users)) != 30 or set(timing_users) != expected_users
                or any(type(stamp) is not int or stamp <= 0 for stamp in posts)):
            raise base.DayStop("short_burst_exact_native_post_timing_unproved")
        rates = burst_rate_metrics(posts)
        maximum = rates["max_posts_per_rolling_second"]
        if maximum > 5:
            raise RateCheckFailed(rates)
        if maximum != 5 or not queued:
            raise base.DayStop("short_burst_native_limiter_binding_not_observed")
        throttle = burst_throttle_proof(rows, begin, self.timeline.cursor.offset)
        state = self.status()
        if not base.ops.ready(state, self.targets, entry=False) or not base.ops.flat(state):
            raise base.DayStop("short_burst_final_complete_flatness_unproved")
        return dict(status="PASS", native_posts=30, **rates,
                    duration_seconds=self.monotonic() - started, limiter_binding_observed=True,
                    queued_native_command_observations=queued, waves=waves,
                    client_order_ids=clients, **proof, **throttle)

    def prepare_empty_closeout(self):
        self.closing = True
        self.closing_phase = "final_cleanup"
        self.command("kill on")
        if self.wall() >= self.window["flat_deadline_utc"]:
            raise base.DayStop("short_flat_deadline_passed")
        result = self.env["flatten"](self.product, self.window["flat_deadline_utc"])
        state = self.status()
        if (result.get("cancel_confirmed") is not True or result.get("flat") is not True
                or self.wall() > self.window["flat_deadline_utc"] or not base.ops.ready(state, self.targets, entry=False)
                or not base.ops.flat(state)):
            raise base.DayStop("short_pre_SIGTERM_complete_flatness_unproved")
        return dict(status="PASS", cancel_confirmed=True, flat=True)

    def restart_case(self):
        # Fault cases deliberately retain kill-on after their recovery. The
        # final restart case must explicitly re-enable entry under the same
        # bounded pre-intent guard before placing its two owned orders.
        self.command("kill off")
        return self.restart_working()

    def independent_case(self, name, callback):
        begin = self.timeline.cursor.offset
        dispatch_begin = self.dispatch_count
        self.active_scenario = name
        self.publish(dict(state="running", phase=name))
        tick_complete = False
        try:
            self.tick()
            tick_complete = True
            result = callback()
        except Exception as error:
            # A failed scenario is retained. Continuing requires the owning
            # supervisor to restore, cancel/reconcile and independently prove
            # current flatness, one owned client and four TEST upstreams.
            dispatched = self.dispatch_count - dispatch_begin
            result = dict(status="FAIL", reason=getattr(error, "code", str(error)),
                          command_not_sent=getattr(error, "not_sent", False) and dispatched == 0,
                          commands_not_sent=getattr(error, "not_sent", False) and dispatched == 0,
                          failing_command_not_sent=getattr(error, "not_sent", False),
                          commands_dispatched_or_uncertain=dispatched)
            if hasattr(error, "metrics"):
                result["metrics"] = error.metrics
            self.mark(name, result, begin)
            fatal = (not tick_complete or (not getattr(error, "not_sent", False)
                     and not isinstance(error, RateCheckFailed) and any(token in str(error).lower()
                     for token in ("uncertain", "mismatch", "identity", "unrestored", "inventory_changed", "limit_exceeded",
                                   "unplanned_link_lost", "native_safety_alert"))))
            if fatal:
                raise
            recover = self.env.get("safe_case_failure")
            if recover is None:
                raise base.DayStop("short_failed_case_safety_recovery_callback_missing") from error
            proof = recover(self.product, error, name)
            if (proof.get("cancel_confirmed") is not True or proof.get("flat") is not True
                    or proof.get("restored") is not True or proof.get("complete_reconstruction") is not True
                    or type(proof.get("client_count")) is not int or proof["client_count"] != 1
                    or type(proof.get("upstreams")) is not int or proof["upstreams"] != 4):
                raise base.DayStop("short_failed_case_safe_continuation_unproved") from error
            self.failed = False
            result["safe_continuation"] = proof
            self.mark(name, result, begin)
            return result
        return self.mark(name, result, begin)

    def run(self):
        self.stage("short_admission", self.admission)
        for name, callback, seconds in (
                ("rate_burst_thirty", self.burst, 120),
                ("one_passive_upstream_socket_loss", lambda: self.fault("one_passive_upstream_socket_loss", "upstream"), 180),
                ("one_passive_router_restart", lambda: self.fault("one_passive_router_restart", "router"), 180),
                ("planned_restart_two_working", self.restart_case, 900)):
            self.independent_case(name, lambda cb=callback, budget=seconds: (self.case_budget(budget), cb())[1])
        self.stage("pre_SIGTERM_cancel_flat", self.prepare_empty_closeout)
        self.stage("terminal_closeout", self.closeout)
        result = "PASS" if all(row.get("status") == "PASS" for row in self.scenarios.values()) else "FAIL"
        return dict(terminal=True, execution_result=result,
                    qualification_result="SUPPLEMENTAL_TEST_COMPLETE" if result == "PASS" else "SUPPLEMENTAL_TEST_INCOMPLETE",
                    coverage_scope="SHORT_INTERRUPTION_ONLY",
                    scenarios=self.scenarios, planned_restarts=int(self.restart_used),
                    journal_bytes=self.timeline.path.stat().st_size,
                    journal_bytes_by_kind=self.timeline.kind_bytes,
                    journal_records_by_kind=self.timeline.kind_counts,
                    native_timing=self.timeline.timing(0), timer_paused=True)


def run(product, cfg, work, env, publish):
    return ShortDriver(product, cfg, work, env, publish).run()


if __name__ == "__main__":
    raise SystemExit("Protected supervisor import only; standalone execution refused.")
