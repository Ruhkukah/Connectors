"""Pure, unarmed T0 preparation checks; this module cannot execute a session.

The returned report establishes document consistency only. Evidence references
are reviewed inputs, not live account, allocation, market or execution proof.
There are deliberately no native, filesystem, scheduler or process interfaces.
"""
from __future__ import annotations

import datetime as dt
import re
from collections.abc import Mapping


SCHEMA = "moex.t0.preparation.v1"
DATE = "2026-10-21"
SOURCE = "0cbc6693916de2a5ccfee1c5f2733128b1cc188d"
BINARY = "4e105a3ccfb4307bf5a679f39aeecbb7cbed889b680b7289342b07678f85c92c"
NATIVE_CI = 38918156841
T0_PROFILE = "7b84fa5d44118dc2ef6fe559174f27f78e362ec568f683ba6bef8f7b5276efe6"
SYMBOLS = ("ALRS-12.26", "CNY-12.26")
LIMITS = dict(quantity=2, position_per_instrument=2, open_orders=5, rate_per_second=5)
QUOTE_POLICY = dict(contracts=2, basis="fresh_conservative_bid", freeze_before_entry=True,
                    raise_during_run=False, request_cap="min(frozen_cap,2*fresh_bid)",
                    closed_market_numeric_caps_allowed=False)
LIVE_REQUIREMENTS = (
    "new_owner_execution_authorization", "verified_complete_reservation_ledger",
    "dedicated_range_allocation_and_checkpoint", "fresh_same_account_complete_flat_snapshot",
    "sole_instance_and_owned_router", "fresh_product_status_admission_and_two_sided_books",
    "fresh_fixed_quote_caps_and_effective_limits", "verified_clock_and_native_provenance",
    "independent_guardian_and_cleanup_ownership",
)


def _require(condition, code):
    if not condition:
        raise ValueError(code)


def _exact_fields(value, fields, code):
    _require(isinstance(value, Mapping) and set(value) == set(fields), code)


def _exact_int(value, expected, code):
    _require(type(value) is int and value == expected, code)


def _digest(value, code):
    _require(type(value) is str and re.fullmatch(r"[0-9a-f]{64}", value) is not None, code)


def _range(value, code):
    _require(isinstance(value, Mapping), code)
    begin, end = value.get("begin"), value.get("end")
    _require(type(begin) is int and type(end) is int and 1 <= begin <= end <= 2_147_483_647, code)
    return begin, end


def make_preparation(begin, end, *, mapping_section, mapping_evidence_sha256,
                     reservation_evidence_sha256):
    """Build a review document, without reserving IDs or authorizing execution.

    ``mapping_section`` belongs in private preparation material. The validator's
    public report deliberately does not return it, account IDs, or input rows.
    """
    return dict(
        schema=SCHEMA, status="PREPARED_UNARMED", venue="T0", environment="test",
        authorized_date=DATE, timezone="Europe/Moscow", owner_authorized=False,
        armed=False, allow_orders=False, review_only=True, timer_paused=True,
        native=dict(source=SOURCE, binary_sha256=BINARY, ci_run_id=NATIVE_CI),
        t0_profile_sha256=T0_PROFILE, symbols=list(SYMBOLS), limits=dict(LIMITS),
        partial_fill_enabled=False,
        id_plan=dict(begin=begin, end=end, planned_starts=2, block_size=1000,
                     before_gate=9000, after_gate=8000, fault_gate=7000, allocated=False,
                     reservations_complete=True,
                     reservation_evidence_sha256=reservation_evidence_sha256),
        mapping=dict(venue="T0", section=mapping_section, account_match=True,
                     evidence_sha256=mapping_evidence_sha256),
        quote_cap_policy=dict(QUOTE_POLICY), strict_rate_window_ns=1_000_000_000,
        rate_slack_ns=0, execution_readiness="NOT_CHECKED",
        fresh_flatness="REQUIRED_BEFORE_ENTRY",
    )


def validate_preparation(document, reservations):
    """Validate a proposal against *all* supplied inclusive reservation ranges.

    No reservation is filtered by account, instance or venue: overlap across
    accounts is conservatively refused. A supplied complete ledger still needs
    independent verification before any future allocation. Preparation never
    produces an arming or trading permission, even with old flatness evidence.
    Error messages contain static codes only, never sensitive input values.
    """
    top_fields = ("schema", "status", "venue", "environment", "authorized_date", "timezone",
                  "owner_authorized", "armed", "allow_orders", "review_only", "timer_paused",
                  "native", "t0_profile_sha256", "symbols", "limits", "partial_fill_enabled",
                  "id_plan", "mapping", "quote_cap_policy", "strict_rate_window_ns",
                  "rate_slack_ns", "execution_readiness", "fresh_flatness")
    _exact_fields(document, top_fields, "preparation_schema_fields")
    for key, expected in (("schema", SCHEMA), ("status", "PREPARED_UNARMED"), ("venue", "T0"),
                          ("environment", "test"), ("timezone", "Europe/Moscow")):
        _require(type(document[key]) is str and document[key] == expected, "preparation_scope")
    _require(type(document["authorized_date"]) is str and document["authorized_date"] == DATE,
             "preparation_calendar_date")
    _require(dt.date.fromisoformat(document["authorized_date"]).weekday() == 2,
             "preparation_calendar_weekday")
    for key in ("owner_authorized", "armed", "allow_orders", "partial_fill_enabled"):
        _require(document[key] is False, "preparation_execution_disabled")
    for key in ("review_only", "timer_paused"):
        _require(document[key] is True, "preparation_review_lock")
    _require(document["execution_readiness"] == "NOT_CHECKED"
             and document["fresh_flatness"] == "REQUIRED_BEFORE_ENTRY", "preparation_live_evidence_not_granted")
    _exact_fields(document["native"], ("source", "binary_sha256", "ci_run_id"), "preparation_native_fields")
    _require(document["native"]["source"] == SOURCE and document["native"]["binary_sha256"] == BINARY,
             "preparation_native_pin")
    _exact_int(document["native"]["ci_run_id"], NATIVE_CI, "preparation_native_ci")
    _require(document["t0_profile_sha256"] == T0_PROFILE, "preparation_profile_pin")
    _require(type(document["symbols"]) in (list, tuple)
             and tuple(document["symbols"]) == SYMBOLS, "preparation_instruments")
    _exact_fields(document["limits"], LIMITS, "preparation_limit_fields")
    for key, expected in LIMITS.items():
        _exact_int(document["limits"][key], expected, "preparation_limit_value")
    _exact_fields(document["quote_cap_policy"], QUOTE_POLICY, "preparation_quote_policy_fields")
    for key, expected in QUOTE_POLICY.items():
        actual = document["quote_cap_policy"][key]
        _require(type(actual) is type(expected) and actual == expected, "preparation_quote_policy")
    _exact_int(document["strict_rate_window_ns"], 1_000_000_000, "preparation_strict_rate_window")
    _exact_int(document["rate_slack_ns"], 0, "preparation_zero_rate_slack")
    mapping = document["mapping"]
    _exact_fields(mapping, ("venue", "section", "account_match", "evidence_sha256"), "preparation_mapping_fields")
    _require(mapping["venue"] == "T0" and mapping["account_match"] is True, "preparation_mapping_account")
    _require(type(mapping["section"]) is str and re.fullmatch(r"[A-Za-z0-9]{7}", mapping["section"])
             is not None, "preparation_mapping_section")
    _digest(mapping["evidence_sha256"], "preparation_mapping_evidence")
    ids = document["id_plan"]
    _exact_fields(ids, ("begin", "end", "planned_starts", "block_size", "before_gate", "after_gate",
                       "fault_gate", "allocated", "reservations_complete", "reservation_evidence_sha256"),
                  "preparation_id_fields")
    begin, end = _range(ids, "preparation_id_range")
    for key, expected in (("planned_starts", 2), ("block_size", 1000), ("before_gate", 9000),
                          ("after_gate", 8000), ("fault_gate", 7000)):
        _exact_int(ids[key], expected, "preparation_id_budget_policy")
    _require(ids["allocated"] is False and ids["reservations_complete"] is True,
             "preparation_id_allocation_or_ledger")
    _digest(ids["reservation_evidence_sha256"], "preparation_reservation_evidence")
    capacity = end - begin + 1
    _require(capacity >= ids["before_gate"]
             and capacity - ids["block_size"] >= ids["after_gate"]
             and capacity - ids["planned_starts"] * ids["block_size"] >= ids["fault_gate"],
             "preparation_id_capacity")
    _require(type(reservations) in (list, tuple), "preparation_reservation_ledger")
    for row in reservations:
        other_begin, other_end = _range(row, "preparation_reserved_range")
        _require(end < other_begin or begin > other_end, "preparation_id_overlap")
    return dict(status="PREPARED_UNARMED", venue="T0", date=DATE, can_arm=False, can_trade=False,
                document_consistent=True, native_source=SOURCE, binary_sha256=BINARY,
                native_ci_run_id=NATIVE_CI, t0_profile_sha256=T0_PROFILE,
                limits=dict(LIMITS), proposed_id_capacity=capacity,
                planned_starts=2, remaining_after_first_start=capacity - 1000,
                remaining_after_planned_starts=capacity - 2000,
                reservations_checked=len(reservations), supplied_ranges_nonoverlapping=True,
                reservation_evidence_reference_sha256=ids["reservation_evidence_sha256"],
                allocation_verified=False, mapping_section_valid=True,
                mapping_evidence_reference_sha256=mapping["evidence_sha256"],
                live_flatness_verified=False, live_market_readiness_verified=False,
                strict_rate_window_ns=1_000_000_000, rate_slack_ns=0,
                required_before_execution=list(LIVE_REQUIREMENTS))
