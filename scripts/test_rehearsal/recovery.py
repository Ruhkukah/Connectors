"""Bounded status-only recovery for the authorized rehearsal driver."""

import time


class RecoveryStop(RuntimeError):
    pass


def owned_signature(state):
    """Called only after the product's complete reconstruction gate passes."""
    if type(state.get("queued")) is not int or state["queued"] != 0 or state.get("reconstructing") is not False:
        raise RecoveryStop("recovery_uncertain_command_or_reconstruction")
    orders, positions = state.get("orders"), state.get("positions")
    if not isinstance(orders, list) or not isinstance(positions, list):
        raise RecoveryStop("recovery_owned_inventory_missing")
    owned = []
    for row in orders:
        if row.get("instance_owned") is not True or row.get("operator_action_required") is not False:
            raise RecoveryStop("recovery_order_ownership_or_outcome_unproved")
        fields = ("client_order_id", "order_id", "isin_id", "side", "price", "quantity", "executed", "remaining", "state")
        if any(k not in row for k in fields) or row["state"] == "Unknown":
            raise RecoveryStop("recovery_order_identity_missing")
        owned.append(tuple(str(row[k]) for k in fields))
    pos = []
    for row in positions:
        fields = ("isin_id", "xpos", "buys", "sells", "last_deal_id")
        if any(type(row.get(k)) is not int for k in fields):
            raise RecoveryStop("recovery_position_identity_missing")
        # Complete absent POS and an explicit all-zero row describe the same inventory.
        values = tuple(row[k] for k in fields)
        if any(values[1:]):
            pos.append(values)
    return tuple(sorted(owned)), tuple(sorted(pos))


def recover(baseline, read_status, complete, tick, *, elapsed=0.0,
            monotonic=time.monotonic, sleep=time.sleep):
    """No order, cancel, restart or readiness override is available here."""
    if baseline is None or not complete(baseline):
        raise RecoveryStop("pre_gap_complete_inventory_unproved")
    expected = owned_signature(baseline)
    started = monotonic()
    deadline = started + max(0.0, 30.0 - elapsed)
    while monotonic() < deadline:
        tick()
        state = read_status()
        if any(state.get(k) for k in ("journal_failed", "cancel_only", "log_error",
                                     "cgate_key_check_failed", "operator_action_required")):
            raise RecoveryStop("recovery_native_safety_alert")
        if complete(state):
            if monotonic() >= deadline:
                break
            if owned_signature(state) != expected:
                raise RecoveryStop("recovered_own_orders_or_positions_differ")
            return state, {"status": "RECOVERED", "elapsed_seconds": elapsed + monotonic() - started,
                           "complete_reconstruction": True, "owned_inventory_unchanged": True,
                           "read_only": True}
        sleep(min(0.1, max(0.0, deadline - monotonic())))
    raise RecoveryStop("private_reconstruction_not_complete_within_30s")
