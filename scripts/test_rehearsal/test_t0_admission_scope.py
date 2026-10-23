import copy
import unittest

from t0_admission_scope import validate_preentry
from t0_preparation import BINARY, DATE, SOURCE, T0_PROFILE


class Preentry(unittest.TestCase):
    def setUp(self):
        self.now = 1792562400_000000000  # 21 October, same authorized date.
        self.cfg = dict(target_venue='T0', authorized_date=DATE, source_git_sha=SOURCE,
            binary_sha256=BINARY, router_profile_sha256=T0_PROFILE,
            account_mapping_sha256='a' * 64, assignment_sha256='b' * 64,
            prearm_flatness_receipt_sha256='c' * 64, prearm_current_flatness_verified=True,
            fresh_T0_preentry_verification_required=True)
        self.receipt = dict(schema='MOEX_T0_PREENTRY_V1', target_venue='T0', authorized_date=DATE,
            native_source=SOURCE, binary_sha256=BINARY, router_profile_sha256=T0_PROFILE,
            account_mapping_sha256='a' * 64, assignment_sha256='b' * 64,
            checked_utc_ns=self.now, same_account_mapping_verified=True,
            old_products_stopped=True, sole_T0_router_verified=True, checkpoint_unchanged=True,
            complete_replication_streams=8, working_orders=0, nonzero_positions=0,
            snapshot_exit=0, snapshot_clients_after=0, router_client_count=0,
            upstream_count=4, publisher_posts=0)

    def verify(self, receipt=None, cfg=None, now=None, actual='c' * 64):
        return validate_preentry(receipt or self.receipt, cfg or self.cfg, actual,
                                 self.now if now is None else now)

    def test_current_same_account_complete_flat_evidence_passes_without_relabelling_prior_run(self):
        before = copy.deepcopy((self.receipt, self.cfg))
        self.assertTrue(self.verify())
        self.assertEqual(before, (self.receipt, self.cfg))
        self.assertNotIn('predecessor_clean_terminal_verified', self.cfg)

    def test_each_missing_false_or_nonzero_proof_refuses(self):
        for key in ('same_account_mapping_verified', 'old_products_stopped',
                    'sole_T0_router_verified', 'checkpoint_unchanged'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.verify(dict(self.receipt, **{key: False}))
        for key in ('working_orders', 'nonzero_positions', 'snapshot_exit',
                    'snapshot_clients_after', 'router_client_count', 'publisher_posts'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.verify(dict(self.receipt, **{key: 1}))
        for key in ('complete_replication_streams', 'upstream_count'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.verify(dict(self.receipt, **{key: 0}))

    def test_stale_future_wrong_date_or_digest_refuses(self):
        for now in (self.now + 600_000_000_001, self.now - 1):
            with self.subTest(now=now), self.assertRaises(ValueError):
                self.verify(now=now)
        for key in ('account_mapping_sha256', 'assignment_sha256', 'router_profile_sha256', 'native_source'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.verify(dict(self.receipt, **{key: '0' * 64}))
        with self.assertRaises(ValueError):
            self.verify(actual='d' * 64)

    def test_integer_zero_and_explicit_sealed_requirement_are_mandatory(self):
        with self.assertRaises(ValueError):
            self.verify(dict(self.receipt, snapshot_exit=False))
        for key in ('fresh_T0_preentry_verification_required', 'prearm_current_flatness_verified'):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.verify(cfg=dict(self.cfg, **{key: False}))


if __name__ == '__main__':
    unittest.main()
