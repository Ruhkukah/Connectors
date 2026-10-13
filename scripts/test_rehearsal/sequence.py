"""Product CLI-only TEST sequence. The protected deployment owns startup and restart.

cmd(line) must invoke actual moexctl plaza2 cmd, return its decoded JSON, and
raise on any nonzero/timeout/uncertain result. status() returns native status.
No function retries a place command. work holds only private host evidence.
"""
import json
import time
import uuid
import price_caps
from native_evidence import CompactReplayVerifier, EvidenceError
from pathlib import Path

TERMINAL = {"Filled", "Cancelled", "Rejected"}
WORKING = {"Working", "PartFilled"}
SCALE = 100000


class SequenceStop(RuntimeError):
    def __init__(self, code, detail=""):
        self.code, self.detail = code, detail
        super().__init__(code + (": " + detail if detail else ""))


def _units(text):
    from decimal import Decimal
    value = Decimal(str(text)) * SCALE
    if value != value.to_integral_value():
        raise SequenceStop("off_grid_decimal")
    return int(value)


def _price(units):
    return f"{units // SCALE}.{units % SCALE:05d}".rstrip("0").rstrip(".")


class _Sequence:
    def __init__(self, cmd, status, work, expected_targets, stage):
        self.cmd, self.status = cmd, status
        self.work = Path(work)
        if not self.work.is_dir():
            raise SequenceStop("private_work_missing")
        self.targets = list(expected_targets)
        if {x["symbol"] for x in self.targets} != {"ALRS-12.26", "CNY-12.26"} or len(self.targets) != 2:
            raise SequenceStop("unexpected_rehearsal_targets")
        self.prefix = "q-" + stage + "-" + uuid.uuid4().hex[:10]
        self.counter = 0
        self.transcript = self.work / "sequence-private.ndjson"
        self.journal = self.work / "interaction.ndjson"

    def record(self, event, data):
        with self.transcript.open("a") as out:
            out.write(json.dumps({"utc_ns": time.time_ns(), "event": event, "data": data}) + "\n")
        self.transcript.chmod(0o600)

    def snapshot(self):
        try:
            value = self.status()
        except Exception as error:
            raise SequenceStop("status_uncertain", type(error).__name__) from error
        self.record("status", value)
        if not isinstance(value, dict):
            raise SequenceStop("status_not_json")
        if value.get("journal_failed") or value.get("log_error") or value.get("cancel_only"):
            raise SequenceStop("product_storage_failure")
        if value.get("cgate_key_check_failed") or value.get("operator_action_required"):
            raise SequenceStop("product_operator_action_required")
        if not value.get("configuration", {}).get("allow_orders"):
            raise SequenceStop("orders_not_enabled")
        config, risk = value["configuration"], value["configuration"]["risk"]
        if not config["sole_instance"] or not config["login_configured"] or config["rate"] != 5 or not 0 < risk["max_quantity"] <= 2 or not 0 < risk["max_open_orders"] <= 5:
            raise SequenceStop("effective_config_outside_approved_limits")
        if any(x["client_order_id"].startswith("q-passive-") and x["executed"] for x in value["orders"]):
            raise SequenceStop("unexpected_passive_fill")
        return value

    def target(self, state, target, require_ready=True):
        rows = [x for x in state["instruments"] if x["isin_id"] == target["isin_id"]]
        if len(rows) != 1 or rows[0]["symbol"] != target["symbol"]:
            raise SequenceStop("refdata_identity_changed")
        row = rows[0]
        if require_ready and (state["reconstructing"] or not row["order_entry_ready"] or not row["book"]["valid"]):
            raise SequenceStop("instrument_not_ready", target["symbol"])
        return row

    def position(self, state, isin):
        rows = [x for x in state["positions"] if x["isin_id"] == isin]
        if len(rows) > 1:
            raise SequenceStop("ambiguous_position")
        return rows[0] if rows else {"xpos": 0, "last_deal_id": 0, "buys": 0, "sells": 0}

    def order(self, state, client_id):
        rows = [x for x in state["orders"] if x["client_order_id"] == client_id]
        if len(rows) > 1:
            raise SequenceStop("duplicate_client_identity")
        if rows and (not rows[0]["instance_owned"] or rows[0]["operator_action_required"] or rows[0]["state"] == "Unknown"):
            raise SequenceStop("order_identity_unresolved", client_id)
        return rows[0] if rows else None

    def wait(self, predicate, code, timeout=60):
        deadline = time.monotonic() + timeout
        while True:
            state = self.snapshot()
            value = predicate(state)
            if value:
                return state, value
            if time.monotonic() >= deadline:
                raise SequenceStop(code)
            time.sleep(0.2)

    def send(self, line):
        self.record("command_requested", line)
        try:
            result = self.cmd(line)
        except Exception as error:
            self.record("command_uncertain", {"line": line, "exception": type(error).__name__})
            raise SequenceStop("command_outcome_uncertain", line) from error
        self.record("command_result", result)
        if not isinstance(result, dict) or not result.get("ok"):
            raise SequenceStop("command_refused", str(result.get("error", "") if isinstance(result, dict) else "non-json"))
        time.sleep(0.22)

    def next_id(self, target):
        self.counter += 1
        return f"{self.prefix}-{target['isin_id']}-{self.counter}"

    def valid_price(self, state, target, units, quantity):
        row = self.target(state, target)
        terms = row["session_terms"]
        if not terms or not terms["interval_valid"] or terms["sess_id"] != state["sess_id"]:
            raise SequenceStop("current_terms_missing")
        step, lower, upper = terms["min_step_scaled"], terms["lower_price_scaled"], terms["upper_price_scaled"]
        limits = next((x for x in state["configuration"]["risk"]["instruments"] if x["isin_id"] == target["isin_id"]), None)
        cap = limits["max_quote_notional_scaled"] if limits else None
        reference = terms["reference_price_scaled"]
        if not limits or not 0 < limits["max_position"] <= 2 or reference is None or cap is None or not 0 < cap <= price_caps.approved_ceiling(target, terms):
            raise SequenceStop("instrument_limits_outside_approved_scope")
        if 'quote_reference_scaled' in target:
            try:
                current = price_caps.current_reference(state, target)
            except ValueError as error:
                raise SequenceStop(str(error)) from error
            if quantity * units > min(cap, 2 * current):
                raise SequenceStop("quote_cap_exceeded", target["symbol"])
        if not step or lower is None or upper is None or cap is None or units % step or not lower <= units <= upper or quantity * units > cap:
            raise SequenceStop("price_outside_native_limits", target["symbol"])
        return row

    def crossing(self, state, target, side, quantity):
        row = self.target(state, target)
        level = row["book"]["best_ask" if side == "buy" else "best_bid"]
        if not level or level["quantity"] < quantity:
            raise SequenceStop("crossing_quote_unavailable", target["symbol"])
        units = level["price_scaled"]
        self.valid_price(state, target, units, quantity)
        return units

    def passive(self, state, target, quantity):
        row = self.target(state, target)
        terms, bid = row["session_terms"], row["book"]["best_bid"]
        if not terms or not bid:
            raise SequenceStop("passive_quote_unavailable")
        step, lower, reference = terms["min_step_scaled"], terms["lower_price_scaled"], terms["reference_price_scaled"]
        if not step or lower is None or reference is None:
            raise SequenceStop("passive_terms_missing")
        price = ((lower + step - 1) // step + 1) * step
        distance = max(20 * step, reference // 100)
        if price > bid["price_scaled"] - distance:
            raise SequenceStop("passive_price_not_far_enough", target["symbol"])
        self.valid_price(state, target, price, quantity)
        return price

    def accepted_order(self, target, client_id, allowed_fill=0):
        def ready(state):
            row = self.order(state, client_id)
            if row and row["state"] == "Rejected":
                raise SequenceStop("exchange_order_rejected", row["last_error"])
            if row and row["executed"] > allowed_fill:
                raise SequenceStop("unexpected_passive_fill", client_id)
            return row if row and row["state"] in WORKING and row["order_id"] > 0 else False
        return self.wait(ready, "add_confirmation_timeout")[1]

    def cancel(self, client_id, allowed_fill=0):
        self.send("cancel " + client_id)
        def done(state):
            row = self.order(state, client_id)
            if row and row["executed"] > allowed_fill:
                raise SequenceStop("unexpected_passive_fill", client_id)
            return row if row and row["state"] in TERMINAL and row["remaining"] == 0 else False
        return self.wait(done, "cancel_not_confirmed")[1]

    def journal_trades(self, offset, client_id, order_id):
        rows = []
        with self.journal.open() as source:
            source.seek(offset)
            for line in source:
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    break  # Buffered final line is not evidence yet.
                data = event.get("data", {})
                if event.get("event") == "trade" and (data.get("client_order_id") == client_id or data.get("order_id") == order_id):
                    rows.append(data)
        return rows

    def mass_cancel(self, target):
        offset = self.journal.stat().st_size
        self.send(f"cancel-all {target['isin_id']}")
        def settled(state):
            if state["queued"] or any(x["state"] not in TERMINAL for x in state["orders"]):
                return False
            commands, acknowledged = set(), False
            with self.journal.open() as source:
                source.seek(offset)
                for line in source:
                    try:
                        event = json.loads(line)
                    except json.JSONDecodeError:
                        break
                    data = event.get("data", {})
                    if event["event"] == "cancel_all_empty" and data.get("isin_id") == target["isin_id"] and data.get("snapshot_confirmed") is True:
                        # A fresh native proven-empty scope sends no exchange
                        # command, so there can be no corresponding reply.
                        # Keep current reconstruction gates as corroboration.
                        from ops import ready
                        if ready(state, self.targets, entry=False):
                            return True
                    elif event["event"] == "command" and data.get("name") == "DelUserOrders" and data.get("fields", {}).get("isin_id") == target["isin_id"]:
                        commands.add(data["user_id"])
                    elif event["event"] == "reply" and data.get("user_id") in commands and data.get("msgid") == 186 and data.get("code") == 0:
                        acknowledged = True
                    elif acknowledged and event["event"] == "transaction_commit" and data.get("stream") == "FORTS_TRADE_REPL":
                        return True
            return False
        self.wait(settled, "cancel_all_ack_or_later_trade_commit_unproven", timeout=10)

    def ioc(self, target, side, quantity, desired_position):
        state = self.snapshot()
        before = self.position(state, target["isin_id"])
        # Check the opposite quote before acquiring exposure; no guarantee of a future fill.
        self.crossing(state, target, "sell" if side == "buy" else "buy", quantity)
        price = self.crossing(state, target, side, quantity)
        client_id = self.next_id(target)
        offset = self.journal.stat().st_size
        self.send(f"place {client_id} {target['isin_id']} {side} {quantity} {_price(price)} ioc")
        def complete(current):
            order = self.order(current, client_id)
            if order and order["state"] in TERMINAL and order["remaining"] == 0:
                if order["executed"] != quantity:
                    raise SequenceStop("ioc_not_fully_filled", client_id)
                trades = self.journal_trades(offset, client_id, order["order_id"])
                if not trades:
                    return False
                keys = [(x["deal_id"], x["order_id"]) for x in trades]
                if len(set(keys)) != len(keys) or sum(x["quantity"] for x in trades) != quantity:
                    raise SequenceStop("fill_credit_not_once", client_id)
                pos = self.position(current, target["isin_id"])
                counter = "buys" if side == "buy" else "sells"
                if pos["xpos"] == desired_position and pos[counter] >= before[counter] + quantity and pos["last_deal_id"] in {x["deal_id"] for x in trades}:
                    return {"client_order_id": client_id, "order_id": order["order_id"], "ext_id": order["ext_id"], "isin_id": target["isin_id"], "executed": quantity, "xpos": desired_position, "last_deal_id": pos["last_deal_id"], "buys": pos["buys"], "sells": pos["sells"]}
            return False
        return self.wait(complete, "fill_or_position_catchup_timeout")[1]

    def assert_flat(self, state):
        if any(x["xpos"] for x in state["positions"]) or state["queued"] or any(x["state"] not in TERMINAL for x in state["orders"]):
            raise SequenceStop("account_not_flat_or_orders_pending")


def passive_cycles(cmd, status, work, expected_targets, count=20):
    if count < 20:
        raise SequenceStop("at_least_twenty_cycles_required")
    seq = _Sequence(cmd, status, work, expected_targets, "passive")
    seq.assert_flat(seq.snapshot())
    completed = 0
    for target in seq.targets:
        for _ in range(count):
            state = seq.snapshot()
            price, client_id = seq.passive(state, target, 1), seq.next_id(target)
            seq.send(f"place {client_id} {target['isin_id']} buy 1 {_price(price)} day")
            seq.accepted_order(target, client_id)
            seq.cancel(client_id)
            seq.assert_flat(seq.snapshot())
            completed += 1
    return {"passive_cycles_completed": completed, "cycles_per_instrument": count}


def fills(cmd, status, work, expected_targets):
    seq = _Sequence(cmd, status, work, expected_targets, "fill")
    seq.assert_flat(seq.snapshot())
    records = [seq.ioc(target, "buy", 1, 1) for target in seq.targets]
    checkpoint = {"records": records, "trade_replay": seq.snapshot()["trade_replay"], "journal_offset": seq.journal.stat().st_size}
    path = seq.work / "sequence-restart-private.json"
    path.write_text(json.dumps(checkpoint) + "\n")
    path.chmod(0o600)
    return {"one_lot_fills_confirmed": len(records), "position_catchup_confirmed": len(records)}


def post_restart(cmd, status, work, expected_targets):
    seq = _Sequence(cmd, status, work, expected_targets, "restart")
    expected = json.loads((seq.work / "sequence-restart-private.json").read_text())
    def replay_floors(state):
        anchor = state["trade_replay"]["used_anchor"]
        if not anchor or state["trade_replay"]["accepted_lifenum"] != anchor["trades_lifenum"]:
            return {}
        first, opened = {}, False
        with seq.journal.open() as source:
            source.seek(expected["journal_offset"])
            for line in source:
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    break
                data = event.get("data", {})
                if data.get("stream") != "FORTS_TRADE_REPL":
                    continue
                if event["event"] == "listener_open":
                    opened = True
                    first = {}
                table = data.get("table")
                if opened and event["event"] == "stream_row" and table in ("user_deal", "heartbeat") and table not in first:
                    fields = {x["name"]: x for x in data["fields"]}
                    if "replRev" not in fields:
                        raise SequenceStop("replay_revision_missing")
                    first[table] = fields["replRev"]["signed"]
                    if first[table] < anchor["trades_rev"]:
                        raise SequenceStop("restart_replay_below_pos_anchor")
        return first
    def matches(state):
        if state["reconstructing"]:
            return False
        if state["queued"] or any(x["state"] not in TERMINAL for x in state["orders"]):
            return False
        for old in expected["records"]:
            candidates = [x for x in state["orders"] if x["isin_id"] == old["isin_id"] and x["instance_owned"] and (x["client_order_id"] == old["client_order_id"] or x["order_id"] == old["order_id"] or x["ext_id"] == old["ext_id"])]
            if len(candidates) > 1:
                raise SequenceStop("ambiguous_reconstructed_identity")
            order = candidates[0] if candidates else None
            pos = seq.position(state, old["isin_id"])
            # Terminal filled orders may be omitted/renamed by reconstruction.
            if (order and (order["executed"] != old["executed"] or order["remaining"] != 0 or order["state"] not in TERMINAL)) or any(pos[k] != old[k] for k in ("xpos", "last_deal_id", "buys", "sells")):
                return False
        return True
    state, _ = seq.wait(matches, "restart_state_not_reconstructed")
    terminal_omitted = sum(not any(x["isin_id"] == old["isin_id"] and x["instance_owned"] and (x["client_order_id"] == old["client_order_id"] or x["order_id"] == old["order_id"] or x["ext_id"] == old["ext_id"]) for x in state["orders"]) for old in expected["records"])
    verifier = CompactReplayVerifier(seq.journal, expected['journal_offset'])
    def compact_proof(state):
        try:
            verifier.update()
            return verifier.result(state, expected['trade_replay']['used_anchor'])
        except EvidenceError as error:
            raise SequenceStop(str(error)) from error
    first_replay = replay_floors(state)
    compact = compact_proof(state)
    deadline = time.monotonic() + 10
    while not compact['proven'] and time.monotonic() < deadline:
        time.sleep(0.5)
        state = seq.snapshot()
        first_replay = replay_floors(state)
        compact = compact_proof(state)
    # An empty table after an advanced POS floor is missing evidence, not a defect.
    missing_replay = sorted({"user_deal", "heartbeat"} - set(first_replay))
    # Reserved fill cache is const in status and releases on the next risk admission.
    # The opposite IOC is the approved next admission; never wait solely on cache==0.
    for target in seq.targets:
        seq.ioc(target, "sell", 1, 0)
    seq.assert_flat(seq.snapshot())
    return {"restart_positions_match": True, "restart_present_orders_execution_match": True, "restart_terminal_orders_omitted": terminal_omitted, "restart_replay_floor_proven": compact["proven"], "restart_legacy_replay_floor_proven": not missing_replay, "restart_compact_replay": compact, "restart_first_replay": first_replay, "restart_missing_replay_tables": missing_replay, "restart_flatten_fills": len(seq.targets)}


def move_and_finish(cmd, status, work, expected_targets):
    seq = _Sequence(cmd, status, work, expected_targets, "move")
    seq.assert_flat(seq.snapshot())
    moves, partials, filled_before_move = 0, 0, 0
    for target in seq.targets:
        state = seq.snapshot()
        price, client_id = seq.passive(state, target, 2), seq.next_id(target)
        seq.send(f"place {client_id} {target['isin_id']} buy 2 {_price(price)} day")
        def accepted(current):
            order = seq.order(current, client_id)
            if order and order["state"] == "Rejected":
                raise SequenceStop("exchange_order_rejected", order["last_error"])
            return order if order and (order["state"] in WORKING or order["state"] == "Filled") and order["order_id"] > 0 else False
        _, order = seq.wait(accepted, "two_lot_add_not_confirmed")
        observation_deadline = time.monotonic() + 5
        while order["executed"] < 2 and time.monotonic() < observation_deadline:
            time.sleep(0.5)
            current = seq.snapshot()
            order = seq.order(current, client_id)
            if not order or order["state"] not in (WORKING | {"Filled"}):
                raise SequenceStop("two_lot_order_unsettled")
        observed_partial = order["executed"] == 1
        if order["executed"] == 2:
            filled_before_move += 1
        else:
            state = seq.snapshot()
            row = seq.target(state, target)
            replacement = price + row["session_terms"]["min_step_scaled"]
            seq.valid_price(state, target, replacement, 2)
            if replacement >= row["book"]["best_bid"]["price_scaled"]:
                raise SequenceStop("move_price_would_cross")
            seq.send(f"move {client_id} 2 {_price(replacement)}")
            def moved(current):
                row = seq.order(current, client_id)
                return row if row and row["state"] in (WORKING | {"Filled"}) and row["quantity"] == 2 and _units(row["price"]) == replacement and row["order_id"] > 0 else False
            seq.wait(moved, "move_not_confirmed")
            moves += 1
            order = seq.cancel(client_id, allowed_fill=2)
            observed_partial = observed_partial or order["executed"] == 1
        partials += observed_partial
        # A passive fill is permitted in this explicitly approved 2-lot stage.
        def settled(current):
            pos = seq.position(current, target["isin_id"])
            return pos if pos["xpos"] == order["executed"] else False
        _, pos = seq.wait(settled, "passive_fill_position_not_caught_up")
        if not 0 <= pos["xpos"] <= 2:
            raise SequenceStop("unexpected_passive_position")
        for remaining in range(pos["xpos"] - 1, -1, -1):
            seq.ioc(target, "sell", 1, remaining)
        seq.mass_cancel(target)
    seq.assert_flat(seq.snapshot())
    return {"move_confirmed": moves, "partial_fill_observed": partials, "filled_before_move": filled_before_move, "cancel_all_completed": len(seq.targets), "ended_flat": True}


def run_sequence(cmd, status, work, expected_targets, restart=None):
    if not callable(restart):
        raise SequenceStop("restart_callback_required_before_first_order", "use the separate segment functions when root owns restart")
    result = passive_cycles(cmd, status, work, expected_targets)
    result.update(fills(cmd, status, work, expected_targets))
    restart()
    result.update(post_restart(cmd, status, work, expected_targets))
    result.update(move_and_finish(cmd, status, work, expected_targets))
    return result
