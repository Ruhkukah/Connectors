import copy
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import ops
import t0_start_budget as budget
from t0_id_registry import virgin_checkpoint


class T0StartBudgetTests(unittest.TestCase):
    def setUp(self):
        # This campaign is sealed to DATE; fixture execution on later dates
        # must exercise the intended gates rather than fail at its calendar.
        fixture_ns = int(dt.datetime.combine(dt.date.fromisoformat(budget.DATE),
            dt.time(12), dt.timezone(dt.timedelta(hours=3))).timestamp() * 1_000_000_000)
        fixture_clock = patch.object(budget.time, 'time_ns', return_value=fixture_ns)
        fixture_clock.start()
        self.addCleanup(fixture_clock.stop)
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


    def write_json(self, path, value):
        path.write_text(json.dumps(value, sort_keys=True))
        path.chmod(0o600)

    def seal_baseline(self, receipt):
        path = self.root / "start-baseline-private.json"
        self.write_json(path, receipt)
        self.cfg.update(start_baseline_receipt=path.name,
                        start_baseline_receipt_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        self.write_json(self.root / "configuration-private.json", self.cfg)
        self.write_json(self.root / "manifest.json", dict(source_git_sha=budget.SOURCE,
            binary_sha256=self.native_sha, files={path.name:self.cfg["start_baseline_receipt_sha256"],
                "configuration-private.json":hashlib.sha256((self.root/"configuration-private.json").read_bytes()).hexdigest()}))

    def continuation(self):
        predecessor = self.root
        oldmap = predecessor / "mapping-private.json"
        self.write_json(oldmap, dict(login="fixture-login", client_codes=["FZ0001o"], target_rows=[1]))
        self.cfg.update(authorized_date=budget.DATE, target_venue="T0",
                        account_mapping_reference=str(oldmap),
                        account_mapping_sha256=hashlib.sha256(oldmap.read_bytes()).hexdigest())
        self.claim()
        self.checkpoint_after_first()
        self.write_json(predecessor / "configuration-private.json", self.cfg)
        self.write_json(predecessor / "manifest.json", dict(source_git_sha=budget.SOURCE,
            binary_sha256=self.native_sha, files={"configuration-private.json":
                hashlib.sha256((predecessor/"configuration-private.json").read_bytes()).hexdigest()}))
        self.write_json(predecessor / "public.json", dict(terminal=True, product_exit=0,
            closeout=dict(cancel_confirmed=True, flat=True), after=dict(client_count=0), no_guardian_failure=True))
        self.write_json(predecessor / "guardian-done.json", dict(terminal=True,
            state="SUPERVISOR_TERMINAL_VERIFIED", supervisor_terminal_closeout_verified=True,
            cancel_confirmed=True, flat=True, product_exit=0, no_guardian_failure=True,
            after=dict(client_count=0), independent_flatness=dict(complete_replication_streams=8,
                working_orders=0, nonzero_positions=0, exit=0, after=dict(client_count=0))))
        self.root = predecessor / "continuation"
        self.root.mkdir(mode=0o700)
        freshmap = self.root / "mapping-private.json"
        self.write_json(freshmap, dict(login="fixture-login", client_codes=["FZ0001o"], target_rows=[2]))
        self.cfg.update(account_mapping_reference=str(freshmap),
                        account_mapping_sha256=hashlib.sha256(freshmap.read_bytes()).hexdigest())
        receipt = dict(schema=budget.BASELINE_SCHEMA, authorized_date=budget.DATE, target_venue="T0",
            source_git_sha=budget.SOURCE, binary_sha256=self.native_sha, instance=self.cfg["instance"],
            ext_id_range=self.cfg["ext_id_range"], assignment_sha256=self.cfg["assignment_sha256"],
            account_mapping_sha256=self.cfg["account_mapping_sha256"],
            state_path_sha256=hashlib.sha256(str(self.state).encode()).hexdigest(),
            root_path_sha256=hashlib.sha256(str(self.root).encode()).hexdigest(),
            baseline_blocks=1, checkpoint_sha256=hashlib.sha256(self.state.read_bytes()).hexdigest(),
            checkpoint_highwaters=dict(ext=2_000_071_000,user=1001), predecessor_root=str(predecessor),
            predecessor_files={name:hashlib.sha256((predecessor/name).read_bytes()).hexdigest()
                               for name in budget.BASELINE_FILES},
            old_exact_owners_stopped=True, no_native_product_present=True, checked_utc_ns=time.time_ns())
        self.seal_baseline(receipt)
        return predecessor, receipt

    def test_clean_predecessor_allows_two_new_monotonic_starts_not_a_third(self):
        predecessor, receipt = self.continuation()
        oldfiles = {name:(predecessor/name).read_bytes() for name in budget.BASELINE_FILES}
        before = self.state.read_bytes()
        first = self.claim()
        self.assertEqual((first["attempt"],first["remaining_ids_before_start"]),(1,49000))
        self.assertEqual(self.state.read_bytes(), before)
        self.assertEqual(oldfiles,{name:(predecessor/name).read_bytes() for name in budget.BASELINE_FILES})
        raw = self.state.read_bytes().replace(b"2000071000 1001", b"2000072000 2001").split()
        payload = b" ".join(raw[:-1]); checksum = 14695981039346656037
        for byte in payload:
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64)-1)
        self.state.write_bytes(payload+b" "+str(checksum).encode()+b"\n")
        second = self.claim("planned_restart")
        self.assertEqual((second["attempt"],second["remaining_ids_before_start"]),(2,48000))
        with self.assertRaisesRegex(ValueError,"two_attempt_budget_exhausted"):
            self.claim("guardian_recovery")
        rows = [json.loads(line) for line in (self.root/"native-start-attempts.ndjson").read_bytes().splitlines()]
        self.assertEqual(rows[0]["identity"]["start_baseline_receipt_sha256"],self.cfg["start_baseline_receipt_sha256"])

    def test_baseline_needs_manifest_pin_current_scope_and_same_day(self):
        _, receipt = self.continuation()
        for key, value in (("baseline_blocks",True),("old_exact_owners_stopped",1),
                           ("no_native_product_present",False),("authorized_date","2026-10-22"),
                           ("source_git_sha","0"*40),("root_path_sha256","0"*64),
                           ("checkpoint_highwaters",dict(ext=2_000_071_001,user=1001))):
            changed = dict(receipt); changed[key] = value; self.seal_baseline(changed)
            with self.subTest(key=key),self.assertRaises(ValueError):
                self.claim()
        self.seal_baseline(receipt)
        self.write_json(self.root/"manifest.json",dict(source_git_sha=budget.SOURCE,
            binary_sha256=self.native_sha,files={"start-baseline-private.json":"0"*64}))
        with self.assertRaisesRegex(ValueError,"baseline_not_manifest_pinned"):
            self.claim()

    def test_changed_checkpoint_or_predecessor_files_cannot_authorize_continuation(self):
        predecessor, receipt = self.continuation()
        changed = dict(receipt,checkpoint_sha256="0"*64);self.seal_baseline(changed)
        with self.assertRaisesRegex(ValueError,"baseline_checkpoint_changed"):
            self.claim()
        self.seal_baseline(receipt)
        path=predecessor/"public.json";path.write_text(path.read_text()+"\n")
        with self.assertRaisesRegex(ValueError,"baseline_predecessor_changed"):
            self.claim()

    def test_failed_terminal_or_guardian_proof_refuses_even_with_correct_file_hashes(self):
        predecessor, receipt = self.continuation()
        path=predecessor/"guardian-done.json";original=json.loads(path.read_text())
        for key,value in (("no_guardian_failure",False),("flat",1),("product_exit",False)):
            changed=dict(original);changed[key]=value;self.write_json(path,changed)
            receipt["predecessor_files"][path.name]=hashlib.sha256(path.read_bytes()).hexdigest()
            self.seal_baseline(receipt)
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,"clean_terminal_unproved"):
                self.claim()

    def test_parent_ledger_count_and_hash_chain_are_independently_verified(self):
        predecessor, receipt = self.continuation()
        path=predecessor/"native-start-attempts.ndjson";raw=path.read_bytes()
        for changed in (raw+raw,raw.replace(b'"sequence":1',b'"sequence":0')):
            path.write_bytes(changed);receipt["predecessor_files"][path.name]=hashlib.sha256(changed).hexdigest()
            self.seal_baseline(receipt)
            with self.assertRaisesRegex(ValueError,"parent_(start_count|ledger_invalid)"):
                self.claim()

    def test_fresh_mapping_rows_may_differ_but_account_or_login_cannot_change(self):
        _, receipt = self.continuation()
        path=Path(self.cfg["account_mapping_reference"])
        original=json.loads(path.read_text())
        for patchvalue in (dict(login="different-login"),dict(client_codes=["AB0001o"])):
            changed=dict(original,**patchvalue);self.write_json(path,changed)
            self.cfg["account_mapping_sha256"]=hashlib.sha256(path.read_bytes()).hexdigest()
            receipt["account_mapping_sha256"]=self.cfg["account_mapping_sha256"];self.seal_baseline(receipt)
            with self.assertRaisesRegex(ValueError,"mapping_identity_changed"):
                self.claim()

    def test_baseline_receipt_cannot_be_relabelled_after_first_claim(self):
        _, receipt = self.continuation()
        self.claim()
        receipt["checked_utc_ns"] += 1;self.seal_baseline(receipt)
        with self.assertRaisesRegex(ValueError,"ledger_invalid"):
            self.claim("planned_restart")

    def test_nonvirgin_state_without_baseline_and_stale_baseline_refuse(self):
        self.checkpoint_after_first()
        with self.assertRaisesRegex(ValueError,"native_block_continuity"):
            self.claim()
        # The first failed claim writes no row and does not consume IDs.
        self.state.write_bytes(virgin_checkpoint(2_000_070_000))
        predecessor, receipt = self.continuation()
        receipt["checked_utc_ns"] -= 700_000_000_000;self.seal_baseline(receipt)
        with self.assertRaisesRegex(ValueError,"baseline_stale_or_future"):
            self.claim()


if __name__ == "__main__":
    unittest.main()
