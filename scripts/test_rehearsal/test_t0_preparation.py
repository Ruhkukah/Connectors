"""Preparation consistency never becomes native execution authorization."""
import copy
import json
import unittest

import t0_preparation as prep


class T0PreparationTests(unittest.TestCase):
    def proposal(self):
        return prep.make_preparation(200_000, 249_999, mapping_section="AB12345",
                                     mapping_evidence_sha256="a" * 64,
                                     reservation_evidence_sha256="b" * 64)

    def check(self, document=None, reservations=None):
        return prep.validate_preparation(self.proposal() if document is None else document,
                                         [] if reservations is None else reservations)

    def mutate(self, path, value):
        document = self.proposal()
        target = document
        for key in path[:-1]:
            target = target[key]
        target[path[-1]] = value
        return document

    def test_prepared_proposal_is_sanitized_and_never_authorizes_execution(self):
        result = self.check(reservations=[dict(begin=1, end=199_999, account="another-account")])
        self.assertEqual(result["status"], "PREPARED_UNARMED")
        self.assertEqual(result["proposed_id_capacity"], 50_000)
        self.assertEqual(result["remaining_after_planned_starts"], 48_000)
        for key in ("can_arm", "can_trade", "allocation_verified", "live_flatness_verified",
                    "live_market_readiness_verified"):
            self.assertIs(result[key], False)
        output = json.dumps(result)
        self.assertNotIn("AB12345", output)
        self.assertNotIn("another-account", output)
        self.assertIn("new_owner_execution_authorization", result["required_before_execution"])

    def test_no_unknown_fields_can_smuggle_commands_or_old_ready_proof(self):
        for key in ("command", "cron", "readiness", "flat", "prior_terminal", "credentials"):
            document = self.proposal()
            document[key] = True
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "schema_fields"):
                self.check(document)

    def test_removing_any_required_field_refuses(self):
        for key in self.proposal():
            document = self.proposal()
            del document[key]
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "schema_fields"):
                self.check(document)

    def test_scope_and_pins_refuse_t1_prod_later_dates_or_new_candidate(self):
        changes = [("venue", "T1"), ("environment", "prod"), ("authorized_date", "2026-10-22"),
                   ("authorized_date", "2026-10-28"), ("timezone", "UTC"), ("schema", "v2"),
                   ("status", "ARMED"), ("t0_profile_sha256", "c" * 64)]
        changes += [("native", None)]
        for key, value in changes:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.check(self.mutate((key,), value))
        for key, value in (("source", "0" * 40), ("binary_sha256", "0" * 64),
                           ("ci_run_id", True), ("ci_run_id", str(prep.NATIVE_CI))):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.check(self.mutate(("native", key), value))

    def test_flags_use_exact_booleans_and_cannot_enable_partial_or_orders(self):
        for key in ("owner_authorized", "armed", "allow_orders", "partial_fill_enabled"):
            for value in (True, 0, "false", None):
                with self.subTest(key=key, value=value), self.assertRaisesRegex(ValueError, "execution_disabled"):
                    self.check(self.mutate((key,), value))
        for key in ("review_only", "timer_paused"):
            for value in (False, 1, "true", None):
                with self.subTest(key=key, value=value), self.assertRaisesRegex(ValueError, "review_lock"):
                    self.check(self.mutate((key,), value))

    def test_exact_limits_reject_bool_fraction_strings_and_limit_enlargement(self):
        for key, expected in prep.LIMITS.items():
            for value in (True, False, float(expected), str(expected), expected + 1, expected - 1):
                with self.subTest(key=key, value=value), self.assertRaisesRegex(ValueError, "limit_value"):
                    self.check(self.mutate(("limits", key), value))

    def test_only_declared_instruments_are_accepted(self):
        for value in (("CNY-12.26", "ALRS-12.26"), ["ALRS-12.26"],
                      ["ALRS-12.26", "CNY-12.26", "BR-12.26"], "ALRS-12.26,CNY-12.26"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "instruments"):
                self.check(self.mutate(("symbols",), value))
        self.check(self.mutate(("symbols",), tuple(prep.SYMBOLS)))

    def test_frozen_quote_policy_rejects_numeric_or_raising_caps(self):
        for key, value in (("contracts", 10), ("contracts", True), ("basis", "ask"),
                           ("freeze_before_entry", False), ("raise_during_run", True),
                           ("closed_market_numeric_caps_allowed", True), ("request_cap", "frozen_cap")):
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "quote_policy"):
                self.check(self.mutate(("quote_cap_policy", key), value))

    def test_strict_f8_window_has_zero_slack(self):
        for path, values in (("strict_rate_window_ns", (999_000_000, 1_000_000_000.0, True)),
                             ("rate_slack_ns", (1, 1_000_000, False))):
            for value in values:
                with self.subTest(path=path, value=value), self.assertRaises(ValueError):
                    self.check(self.mutate((path,), value))

    def test_inclusive_overlap_catches_both_touching_endpoints_and_cross_account(self):
        for begin, end in ((199_999, 200_000), (249_999, 250_000), (220_000, 220_001),
                           (1, 999_999), (200_000, 249_999)):
            with self.subTest(begin=begin, end=end), self.assertRaisesRegex(ValueError, "id_overlap"):
                self.check(reservations=[dict(begin=begin, end=end, venue="T1", account="OTHER")])
        self.check(reservations=[dict(begin=1, end=199_999), dict(begin=250_000, end=300_000)])

    def test_invalid_or_unknown_reservation_rows_do_not_silently_disappear(self):
        for row in (dict(begin=True, end=100), dict(begin=1, end=False), dict(begin=5, end=4),
                    dict(begin=0, end=10), dict(begin=1), None, "not-a-range"):
            with self.subTest(row=row), self.assertRaisesRegex(ValueError, "reserved_range"):
                self.check(reservations=[row])
        with self.assertRaisesRegex(ValueError, "reservation_ledger"):
            prep.validate_preparation(self.proposal(), iter([]))

    def test_range_int32_bounds_and_inclusive_capacity(self):
        document = self.proposal()
        document["id_plan"].update(begin=2_147_483_647 - 8999, end=2_147_483_647)
        result = self.check(document)
        self.assertEqual(result["proposed_id_capacity"], 9000)
        self.assertEqual(result["remaining_after_first_start"], 8000)
        self.assertEqual(result["remaining_after_planned_starts"], 7000)
        for begin, end in ((200_000, 208_998), (0, 50_000), (True, 50_000),
                           (1, 2_147_483_648), (249_999, 200_000)):
            invalid = self.proposal()
            invalid["id_plan"].update(begin=begin, end=end)
            with self.subTest(begin=begin, end=end), self.assertRaises(ValueError):
                self.check(invalid)

    def test_two_start_budget_and_unchanged_gates_are_required(self):
        for key, value in (("planned_starts", 1), ("planned_starts", 3), ("planned_starts", True),
                           ("block_size", 999), ("before_gate", 8999), ("after_gate", 7999),
                           ("fault_gate", 6999), ("allocated", True), ("reservations_complete", False)):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.check(self.mutate(("id_plan", key), value))

    def test_mapping_requires_t0_match_private_section_and_real_evidence_reference(self):
        for key, values in (("venue", ("T1",)), ("account_match", (False, 1)),
                             ("section", ("", "123456", "12345678", "AB/2345", "АБ12345")),
                             ("evidence_sha256", ("", "NO_FILE", "g" * 64))):
            for value in values:
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    self.check(self.mutate(("mapping", key), value))
        for value in (None, "", "c" * 63):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "reservation_evidence"):
                self.check(self.mutate(("id_plan", "reservation_evidence_sha256"), value))

    def test_preparation_cannot_adopt_stale_flatness_or_scheduled_readiness(self):
        for key, value in (("execution_readiness", "READY"), ("fresh_flatness", True),
                           ("fresh_flatness", "yesterday-flat")):
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "live_evidence_not_granted"):
                self.check(self.mutate((key,), value))

    def test_validation_does_not_mutate_evidence_or_input(self):
        document = self.proposal()
        rows = [dict(begin=1, end=199_999, account="PRIVATE")]
        original_document, original_rows = copy.deepcopy(document), copy.deepcopy(rows)
        first = self.check(document, rows)
        self.assertEqual(document, original_document)
        self.assertEqual(rows, original_rows)
        first["limits"]["quantity"] = 999
        self.assertEqual(self.check(document, rows)["limits"]["quantity"], 2)


if __name__ == "__main__":
    unittest.main()
