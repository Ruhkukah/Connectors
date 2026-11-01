"""Pure T0 execution-composition scope, with a separate unarmed review validator.

Importing, preparing a calendar or validating held configuration never grants
execution authority, starts a process or consults the wall clock. Allocation
hashes are references: the installer must verify the actual registered receipt.
"""
from __future__ import annotations

import datetime as dt
import math
import re
from collections.abc import Mapping

from t0_preparation import BINARY, DATE, NATIVE_CI, SOURCE, SYMBOLS, T0_PROFILE

MSK = dt.timezone(dt.timedelta(hours=3))
DATES = frozenset((DATE,))
dates = DATES
ID_BEGIN = 2_000_070_000
ID_END = 2_000_119_999
FAULT_AUTHORIZATION_FLAGS = (
    "restart_two_orders_authorized", "router_stop_start_authorized",
    "upstream_socket_cut_authorized", "rate_burst_authorized",
    "empty_scope_sigterm_authorized", "final_working_sigterm_authorized",
    "one_passive_order_authorized",
)


def _require(condition, code):
    if not condition:
        raise ValueError(code)


def _int(value, expected, code):
    _require(type(value) is int and value == expected, code)


def _hex(value, length, code):
    _require(type(value) is str and re.fullmatch(r"[0-9a-f]{" + str(length) + r"}", value)
             is not None, code)


def epoch(date, hour, minute=0):
    _require(type(date) is str and date in DATES, "t0_execution_calendar_date_unproved")
    _require(type(hour) is int and type(minute) is int, "t0_execution_clock_type")
    try:
        return dt.datetime.combine(dt.date.fromisoformat(date), dt.time(hour, minute), MSK).timestamp()
    except ValueError:
        raise ValueError("t0_execution_clock_value") from None


def expected(date):
    """Frozen schedule only; a returned calendar is not owner authorization.

    Keep this return shape identical to the frozen short-session window fields.
    The later independent postcheck bounds are validated in configuration only.
    """
    _require(type(date) is str and date in DATES, "t0_execution_calendar_date_unproved")
    return dict(authorized_date=date, scheduled_start_utc=epoch(date, 9),
                earliest_entry_utc=epoch(date, 9, 5),
                product_start_latest_utc=epoch(date, 11, 50),
                admission_latest_utc=epoch(date, 12),
                hard_entry_deadline_utc=epoch(date, 14, 30),
                flat_deadline_utc=epoch(date, 14, 40),
                guardian_trigger_utc=epoch(date, 14, 55),
                hard_cleanup_deadline_utc=epoch(date, 15))


def _common(cfg):
    _require(isinstance(cfg, Mapping), "t0_execution_configuration_missing")
    result = expected(cfg.get("authorized_date"))
    for key, value in result.items():
        if key == "authorized_date":
            continue
        actual = cfg.get(key)
        _require(type(actual) in (int, float) and math.isfinite(actual) and actual == value,
                 "t0_execution_sealed_window_changed")
    for key, value in (("post_session_check_utc", epoch(DATE, 15, 10)),
                       ("post_session_check_deadline_utc", epoch(DATE, 15, 40))):
        actual = cfg.get(key)
        _require(type(actual) in (int, float) and math.isfinite(actual) and actual == value,
                 "t0_execution_postcheck_window_changed")
    _require(cfg.get("target_venue") == "T0" and cfg.get("environment") == "test",
             "t0_execution_target_changed")
    _int(cfg.get("router_port"), 4101, "t0_execution_router_port_changed")
    _require(cfg.get("source_git_sha") == SOURCE and cfg.get("binary_sha256") == BINARY,
             "t0_execution_native_pin_changed")
    _int(cfg.get("ci_run_id"), NATIVE_CI, "t0_execution_native_ci_changed")
    _require(cfg.get("t0_profile_sha256") == T0_PROFILE
             and cfg.get("router_profile_sha256") == T0_PROFILE, "t0_execution_profile_changed")
    _hex(cfg.get("review_git_sha"), 40, "t0_execution_review_pin_missing")
    _require(type(cfg.get("review_ci_run_id")) is int and cfg["review_ci_run_id"] > 0,
             "t0_execution_review_ci_missing")
    _require(type(cfg.get("symbols")) in (list, tuple) and tuple(cfg["symbols"]) == SYMBOLS,
             "t0_execution_instruments_changed")
    for key, value in (("quantity_limit", 2), ("position_limit", 2),
                       ("open_order_limit", 5), ("rate_limit", 5),
                       ("strict_rate_window_ns", 1_000_000_000), ("rate_slack_ns", 0),
                       ("planned_starts", 2), ("block_size", 1000),
                       ("before_gate", 9000), ("after_gate", 8000), ("fault_gate", 7000),
                       ("ext_id_begin", ID_BEGIN), ("ext_id_end", ID_END)):
        _int(cfg.get(key), value, "t0_execution_risk_or_id_policy_changed")
    _require(type(cfg.get("ext_id_range")) is str and
             cfg["ext_id_range"] == f"{ID_BEGIN}:{ID_END}", "t0_execution_id_range_changed")
    _require(cfg.get("allocation_registered") is True, "t0_execution_allocation_unregistered")
    _hex(cfg.get("assignment_sha256"), 64, "t0_execution_assignment_pin_missing")
    _require(cfg.get("partial_fill_enabled") is False and cfg.get("latency_gating") is False,
             "t0_execution_optional_scope_changed")
    _require(cfg.get("timer_paused") is True and cfg.get("no_watchdog_sigkill") is True
             and cfg.get("skip_firewall") is True, "t0_execution_safety_policy_changed")
    _require(cfg.get("same_day_continuation", False) is False and
             "continuation_start_utc" not in cfg, "t0_execution_continuation_not_authorized")
    for key in FAULT_AUTHORIZATION_FLAGS:
        _require(type(cfg.get(key)) is bool, "t0_execution_case_authority_missing")
    return result


def validate_held_config(cfg):
    """Review a registered but held package; never evaluate it as executable.

    This validates pins, scope and document consistency, not the contents of the
    referenced allocation or CI receipt, live ownership, flatness or admission.
    """
    _common(cfg)
    _require(all(cfg.get(key) is False for key in ("owner_authorized", "armed", "allow_orders"))
             and cfg.get("review_only") is True, "t0_execution_held_lock_changed")
    return dict(status="COMPOSITION_VALIDATED_UNARMED", can_arm=False, can_trade=False,
                owner_execution_authorized=False, target_venue="T0", authorized_date=DATE,
                native_source=SOURCE, binary_sha256=BINARY, native_ci_run_id=NATIVE_CI,
                review_git_sha=cfg["review_git_sha"], review_ci_run_id=cfg["review_ci_run_id"],
                assignment_reference_sha256=cfg["assignment_sha256"],
                live_flatness_verified=False, live_market_readiness_verified=False,
                id_capacity=ID_END-ID_BEGIN+1, planned_starts=2,
                remaining_after_first_start=ID_END-ID_BEGIN+1-1000,
                remaining_after_planned_starts=ID_END-ID_BEGIN+1-2000)


def window(cfg, *, now=None, entry=False):
    """Validate future explicit execution authorization; held preparation refuses.

    ``now`` is an optional externally sampled UTC epoch. No time is sampled at
    import or validation; callers retain their independent current-day checks.
    """
    _require(isinstance(cfg, Mapping) and cfg.get("owner_authorized") is True and
             cfg.get("armed") is True and cfg.get("allow_orders") is True and
             cfg.get("review_only") is False, "t0_execution_owner_authority_missing")
    result = _common(cfg)
    _require(all(cfg[key] is True for key in FAULT_AUTHORIZATION_FLAGS),
             "t0_execution_case_not_authorized")
    _require(type(entry) is bool, "t0_execution_entry_flag_type")
    _require(not entry or now is not None, "t0_execution_entry_clock_missing")
    if now is not None:
        _require(type(now) in (int, float) and math.isfinite(now), "t0_execution_clock_type")
        try:
            actual_date = dt.datetime.fromtimestamp(now, MSK).date().isoformat()
        except (OverflowError, OSError, ValueError):
            raise ValueError("t0_execution_clock_value") from None
        _require(actual_date == DATE, "t0_execution_current_date_changed")
        if entry:
            _require(result["earliest_entry_utc"] <= now < result["hard_entry_deadline_utc"],
                     "t0_execution_entry_window_closed")
    return result


validate = window
