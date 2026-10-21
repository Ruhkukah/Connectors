import copy
import hashlib
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch

import ops
import t0_start_budget as budget
from t0_id_registry import virgin_checkpoint


class T0StartBudgetTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.binary, self.state, self.assignment = [self.root / name for name in ("candidate", "state", "assignment")]
        self.binary.write_bytes(b"fixture-native-no-execution")
        self.state.write_bytes(virgin_checkpoint(2_000_070_000))
        self.assignment.write_text(json.dumps(dict(instance="t0", ext_id_begin=2_000_070_000,
                                                  ext_id_end=2_000_119_999)))
        for path in (self.binary, self.state, self.assignment):
            path.chmod(0o600)
        self.native_sha = hashlib.sha256(self.binary.read_bytes()).hexdigest()
        native = patch.object(budget, "BINARY", self.native_sha)
        native.start()
        self.addCleanup(native.stop)
        self.cfg = dict(binary=str(self.binary), binary_sha256=self.native_sha,
                        source_git_sha=budget.SOURCE, state=str(self.state), assignment=str(self.assignment),
                        assignment_sha256=hashlib.sha256(self.assignment.read_bytes()).hexdigest(),
                        instance="t0", ext_id_range="2000070000:2000119999", planned_starts=2)
        self.args = [str(self.binary), "plaza2", "run", "--environment", "test", "--allow-orders",
                     "--state", str(self.state), "--ext-id-range", self.cfg["ext_id_range"], "--instance-id", "t0"]

    def claim(self, purpose="initial", **changes):
        values = dict(binary=self.binary, state=self.state, args=self.args, purpose=purpose)
        values.update(changes)
        return budget.claim_start(self.root, self.cfg, **values)

    def checkpoint_after_first(self):
        record = b"MOEXJ2 10 20 0 2000071000 1001 14695981039346656037"
        checksum = 14695981039346656037
        for byte in record:
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
        self.state.write_bytes(record + b" " + str(checksum).encode() + b"\n")

    def test_first_and_only_replacement_consume_two_attempts_not_ids(self):
        before = self.state.read_bytes()
        first = self.claim()
        self.assertEqual(first["attempt"], 1)
        self.assertEqual(first["remaining_ids_before_start"], 50000)
        self.assertEqual(self.state.read_bytes(), before)
        self.checkpoint_after_first()
        second = self.claim("planned_restart")
        self.assertEqual(second["attempt"], 2)
        self.assertEqual(second["remaining_ids_before_start"], 49000)
        with self.assertRaisesRegex(ValueError, "two_attempt_budget_exhausted"):
            self.claim("guardian_recovery")

    def test_guardian_and_planned_restart_compete_for_same_final_attempt(self):
        self.claim()
        self.checkpoint_after_first()
        barrier, results = threading.Barrier(2), []
        def contender(purpose):
            barrier.wait(timeout=5)
            try:
                results.append(self.claim(purpose))
            except ValueError as error:
                results.append(str(error))
        threads = [threading.Thread(target=contender, args=(purpose,)) for purpose in
                   ("planned_restart", "guardian_recovery")]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=10)
            self.assertFalse(thread.is_alive())
        self.assertEqual(sum(isinstance(value, dict) for value in results), 1)
        self.assertIn("t0_start_two_attempt_budget_exhausted", results)

    def test_real_product_failed_Popen_does_not_restore_consumed_attempt(self):
        original = ops.Product.start
        def claimed_start(product):
            self.claim("initial" if not (self.root / "native-start-attempts.ndjson").exists()
                       else "planned_restart")
            return original(product)
        product = ops.Product(self.root, self.binary, {}, self.args, [])
        with patch.object(ops.Product, "start", claimed_start), patch.object(ops.subprocess, "Popen",
                side_effect=OSError("synthetic failed Popen")) as popen:
            with self.assertRaises(OSError):
                product.start()
            self.assertEqual(popen.call_count, 1)
            with self.assertRaisesRegex(ValueError, "native_block_continuity"):
                product.start()
            self.assertEqual(popen.call_count, 1)
            self.checkpoint_after_first()
            with self.assertRaises(OSError):
                product.start()
            self.assertEqual(popen.call_count, 2)
            with self.assertRaisesRegex(ValueError, "two_attempt_budget_exhausted"):
                product.start()
            self.assertEqual(popen.call_count, 2)

    def test_retry_initial_or_replacement_before_native_block_refuses(self):
        with self.assertRaisesRegex(ValueError, "retry_or_purpose"):
            self.claim("guardian_recovery")
        self.claim()
        with self.assertRaisesRegex(ValueError, "retry_or_purpose"):
            self.claim()
        with self.assertRaisesRegex(ValueError, "native_block_continuity"):
            self.claim("planned_restart")

    def test_readonly_or_different_namespace_native_and_assignment_refuse(self):
        for transform in (lambda args: [x for x in args if x != "--allow-orders"],
                          lambda args: args + ["--state", str(self.state)],
                          lambda args: ["prod" if x == "test" else x for x in args],
                          lambda args: ["different" if x == "t0" else x for x in args]):
            with self.subTest(transform=transform), self.assertRaises(ValueError):
                self.claim(args=transform(list(self.args)))
        self.binary.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "identity_changed"):
            self.claim()
        self.assertFalse((self.root / "native-start-attempts.ndjson").exists())

    def test_corrupt_attempt_ledger_cannot_grant_new_start(self):
        self.claim()
        self.checkpoint_after_first()
        ledger = self.root / "native-start-attempts.ndjson"
        old = ledger.read_bytes()
        ledger.write_bytes(old.replace(b'"sequence":1', b'"sequence":0'))
        with self.assertRaisesRegex(ValueError, "ledger_invalid"):
            self.claim("guardian_recovery")
        self.assertEqual(len(ledger.read_bytes().splitlines()), 1)


if __name__ == "__main__":
    unittest.main()
