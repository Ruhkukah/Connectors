"""Review and execution authority stay separate, without native side effects."""
import copy
import datetime as dt
import unittest

import t0_execution_scope as scope
from t0_preparation import BINARY, DATE, NATIVE_CI, SOURCE, SYMBOLS, T0_PROFILE


def held():
    return dict(scope.expected(DATE), target_venue="T0", environment="test", router_port=4101,
                source_git_sha=SOURCE, binary_sha256=BINARY, ci_run_id=NATIVE_CI,
                review_git_sha="a"*40, review_ci_run_id=12345, t0_profile_sha256=T0_PROFILE,
                router_profile_sha256=T0_PROFILE,
                symbols=list(SYMBOLS), quantity_limit=2, position_limit=2, open_order_limit=5,
                rate_limit=5, strict_rate_window_ns=1_000_000_000, rate_slack_ns=0,
                planned_starts=2, block_size=1000, before_gate=9000, after_gate=8000,
                fault_gate=7000, ext_id_begin=scope.ID_BEGIN, ext_id_end=scope.ID_END,
                ext_id_range=f"{scope.ID_BEGIN}:{scope.ID_END}", allocation_registered=True,
                assignment_sha256="b"*64, partial_fill_enabled=False, latency_gating=False,
                timer_paused=True, no_watchdog_sigkill=True, skip_firewall=True,
                owner_authorized=False, armed=False, allow_orders=False, review_only=True,
                post_session_check_utc=scope.epoch(DATE, 15, 10),
                post_session_check_deadline_utc=scope.epoch(DATE, 15, 40),
                **{key: False for key in scope.FAULT_AUTHORIZATION_FLAGS})


def executable():
    cfg = held()
    cfg.update(owner_authorized=True, armed=True, allow_orders=True, review_only=False,
               **{key: True for key in scope.FAULT_AUTHORIZATION_FLAGS})
    return cfg


class T0ExecutionScopeTests(unittest.TestCase):
    def test_held_registered_package_validates_without_execution_authority(self):
        cfg = held()
        original = copy.deepcopy(cfg)
        report = scope.validate_held_config(cfg)
        self.assertEqual(cfg, original)
        self.assertEqual(report["status"], "COMPOSITION_VALIDATED_UNARMED")
        for key in ("can_arm", "can_trade", "owner_execution_authorized",
                    "live_flatness_verified", "live_market_readiness_verified"):
            self.assertIs(report[key], False)
        self.assertEqual(report["id_capacity"], 50000)
        self.assertEqual(report["remaining_after_planned_starts"], 48000)

    def test_held_config_and_calendar_cannot_be_executed(self):
        for cfg in (held(), scope.expected(DATE)):
            with self.subTest(cfg=cfg.keys()), self.assertRaisesRegex(ValueError, "owner_authority"):
                scope.window(cfg)

    def test_held_lock_rejects_preparation_relabelled_as_authorization(self):
        for key in ("owner_authorized", "armed", "allow_orders", "review_only"):
            for value in (True, 1, "false", None):
                if key == "review_only" and value is True:
                    continue
                with self.subTest(key=key, value=value):
                    cfg = held(); cfg[key] = value
                    with self.assertRaisesRegex(ValueError, "held_lock"):
                        scope.validate_held_config(cfg)
        cfg = held(); cfg["review_only"] = False
        with self.assertRaisesRegex(ValueError, "held_lock"):
            scope.validate_held_config(cfg)

    def test_execution_needs_all_four_locks_and_each_case_authority(self):
        good = executable()
        for key in ("owner_authorized", "armed", "allow_orders", "review_only"):
            for value in (None, 0, 1, "true", not good[key]):
                with self.subTest(key=key, value=value):
                    cfg = dict(good); cfg[key] = value
                    with self.assertRaises(ValueError):
                        scope.window(cfg)
        for key in scope.FAULT_AUTHORIZATION_FLAGS:
            cfg = dict(good); cfg[key] = False
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "case_not_authorized"):
                scope.window(cfg)
            cfg[key] = 1
            with self.assertRaisesRegex(ValueError, "case_authority_missing"):
                scope.window(cfg)

    def test_window_return_matches_frozen_short_runtime_shape(self):
        expected = scope.expected(DATE)
        self.assertEqual(set(expected), {"authorized_date", "scheduled_start_utc", "earliest_entry_utc",
            "product_start_latest_utc", "admission_latest_utc", "hard_entry_deadline_utc",
            "flat_deadline_utc", "guardian_trigger_utc", "hard_cleanup_deadline_utc"})
        self.assertEqual(scope.window(executable()), expected)
        self.assertIs(scope.validate, scope.window)
        self.assertEqual(dt.datetime.fromtimestamp(expected["earliest_entry_utc"], scope.MSK).hour, 9)
        self.assertEqual(dt.datetime.fromtimestamp(expected["earliest_entry_utc"], scope.MSK).minute, 5)

    def test_native_and_review_provenance_are_separate(self):
        cfg = held()
        self.assertNotEqual(cfg["review_git_sha"], SOURCE)
        self.assertEqual(scope.validate_held_config(cfg)["native_source"], SOURCE)
        for key, value in (("source_git_sha", "a"*40), ("binary_sha256", "c"*64),
                           ("ci_run_id", True), ("ci_run_id", NATIVE_CI+1),
                           ("review_git_sha", "A"*40), ("review_git_sha", "placeholder"),
                           ("review_ci_run_id", True), ("review_ci_run_id", 0)):
            cfg = held(); cfg[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                scope.validate_held_config(cfg)

    def test_wrong_venue_router_profile_and_instrument_order_refuse(self):
        for key, value in (("target_venue", "T1"), ("environment", "prod"),
                           ("router_port", 4001), ("router_port", True),
                           ("t0_profile_sha256", "d"*64), ("router_profile_sha256", "d"*64),
                           ("symbols", list(reversed(SYMBOLS))), ("symbols", list(SYMBOLS)+["Si-12.26"])):
            cfg = held(); cfg[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                scope.validate_held_config(cfg)

    def test_risk_and_id_policy_are_exact_integers_not_booleans(self):
        keys = ("quantity_limit", "position_limit", "open_order_limit", "rate_limit",
                "strict_rate_window_ns", "rate_slack_ns", "planned_starts", "block_size",
                "before_gate", "after_gate", "fault_gate", "ext_id_begin", "ext_id_end")
        for key in keys:
            for value in (True, False, float(held()[key]), str(held()[key]), held()[key]+1):
                cfg = held(); cfg[key] = value
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    scope.validate_held_config(cfg)

    def test_allocation_requires_actual_assignment_reference_and_exact_range(self):
        for key, value in (("allocation_registered", False), ("allocation_registered", 1),
                           ("assignment_sha256", None), ("assignment_sha256", "a"*63),
                           ("assignment_sha256", "A"*64), ("ext_id_range", "1:50000")):
            cfg = held(); cfg[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                scope.validate_held_config(cfg)

    def test_all_safety_policies_and_optional_scope_are_exact(self):
        for key in ("partial_fill_enabled", "latency_gating", "timer_paused",
                    "no_watchdog_sigkill", "skip_firewall"):
            for value in (not held()[key], int(held()[key]), None):
                cfg = held(); cfg[key] = value
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    scope.validate_held_config(cfg)
        for key in scope.FAULT_AUTHORIZATION_FLAGS:
            cfg = held(); cfg[key] = 0
            with self.assertRaises(ValueError):
                scope.validate_held_config(cfg)

    def test_schedule_missing_mutated_nonfinite_and_bool_refuse(self):
        keys = set(scope.expected(DATE)) - {"authorized_date"}
        keys |= {"post_session_check_utc", "post_session_check_deadline_utc"}
        for key in keys:
            for value in (None, True, float("inf"), float("nan"), held()[key]+60):
                cfg = held(); cfg[key] = value
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    scope.validate_held_config(cfg)

    def test_only_wednesday_no_automatic_next_day_or_continuation(self):
        for date in (None, "2026-10-19", "2026-10-22", "2026-10-28", DATE+"T00:00:00"):
            with self.subTest(date=date), self.assertRaisesRegex(ValueError, "calendar_date"):
                scope.expected(date)
        for patch in ({"same_day_continuation": True}, {"same_day_continuation": 0},
                      {"continuation_start_utc": scope.epoch(DATE, 10)}):
            with self.subTest(patch=patch), self.assertRaisesRegex(ValueError, "continuation"):
                scope.validate_held_config(dict(held(), **patch))

    def test_optional_current_day_and_entry_checks_use_supplied_clock_only(self):
        cfg = executable()
        self.assertEqual(scope.window(cfg, now=scope.epoch(DATE, 9)), scope.expected(DATE))
        for instant in (scope.epoch(DATE, 9, 4), scope.epoch(DATE, 14, 30)):
            with self.assertRaisesRegex(ValueError, "entry_window_closed"):
                scope.window(cfg, now=instant, entry=True)
        for instant in (scope.epoch(DATE, 9, 5), scope.epoch(DATE, 14, 29)):
            self.assertEqual(scope.window(cfg, now=instant, entry=True), scope.expected(DATE))
        with self.assertRaisesRegex(ValueError, "entry_clock_missing"):
            scope.window(cfg, entry=True)
        with self.assertRaisesRegex(ValueError, "current_date_changed"):
            scope.window(cfg, now=scope.epoch(DATE, 9)+86400)
        for instant in (True, float("nan"), "now"):
            with self.assertRaisesRegex(ValueError, "clock_type"):
                scope.window(cfg, now=instant)
        with self.assertRaisesRegex(ValueError, "entry_flag_type"):
            scope.window(cfg, now=scope.epoch(DATE, 9), entry=1)


if __name__ == "__main__":
    unittest.main()
