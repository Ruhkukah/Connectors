"""Allocation consistency, corruption refusal and concurrent registration."""
import os
import json
from pathlib import Path
import tempfile
import threading
import unittest

import t0_id_registry as registry


class T0IDRegistryTests(unittest.TestCase):
    def proposal(self, **changes):
        value = dict(instance="moex_t0_qualification_20261021", ext_id_begin=2_000_070_000,
                     ext_id_end=2_000_119_999, venue="T0", purpose="qualification")
        value.update(changes)
        return value

    def snapshot(self, **changes):
        value = dict(complete=True, evidence_sha256="a" * 64,
                     entries=[dict(instance="old_t1", ext_id_begin=1, ext_id_end=100_000)])
        value.update(changes)
        return value

    def call(self, folder, proposal=None, loader=None):
        return registry.register_range(Path(folder) / "ledger.ndjson", proposal or self.proposal(),
                                       historical_loader=loader or self.snapshot,
                                       lock_path=Path(folder) / "allocation.lock")

    def test_registers_dedicated_range_without_consumption_or_execution(self):
        with tempfile.TemporaryDirectory() as folder:
            result = self.call(folder)
            self.assertTrue(result["appended"])
            for key in ("armed", "can_trade"):
                self.assertIs(result[key], False)
            self.assertEqual(result["ids_consumed"], 0)
            record = result["receipt"]
            self.assertEqual(record["registration"], self.proposal())
            self.assertEqual(record["authority"]["range_count"], 1)
            self.assertEqual(record["authority"]["entries_sha256"], registry.digest(self.snapshot()["entries"]))
            self.assertEqual({path.name for path in Path(folder).iterdir()}, {"ledger.ndjson", "allocation.lock"})
            self.assertTrue(all(path.stat().st_mode & 0o777 == 0o600 for path in Path(folder).iterdir()))

    def test_inclusive_endpoint_overlap_and_existing_instance_refuse(self):
        for begin, end in ((2_000_069_999, 2_000_070_000), (2_000_119_999, 2_000_120_000),
                           (2_000_080_000, 2_000_080_001), (1, 2_147_483_647)):
            with self.subTest(begin=begin), self.assertRaisesRegex(ValueError, "overlap"):
                registry.validate_registration(self.proposal(),
                    [dict(instance="another_venue", ext_id_begin=begin, ext_id_end=end)])
        with self.assertRaisesRegex(ValueError, "instance_reuse"):
            registry.validate_registration(self.proposal(),
                [dict(instance=self.proposal()["instance"], ext_id_begin=1, ext_id_end=10000)])

    def test_historical_extensions_are_checked_without_false_mutual_overlap_failure(self):
        entries = self.snapshot()["entries"] + [dict(instance="old_t1", ext_id_begin=1, ext_id_end=150_000)]
        result = registry.validate_registration(self.proposal(), entries)
        self.assertEqual(result["historical_ranges_checked"], 2)

    def test_bool_ranges_int32_bounds_and_two_start_budget_refuse(self):
        for changes in (dict(ext_id_begin=True), dict(ext_id_end=2_147_483_648),
                        dict(ext_id_begin=0), dict(ext_id_end=2_000_069_999),
                        dict(ext_id_end=2_000_078_998), dict(venue="T1"), dict(purpose="trading"),
                        dict(instance="secret account value"), dict(armed=True)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                registry.validate_registration(self.proposal(**changes), [])
        result = registry.validate_registration(self.proposal(ext_id_end=2_000_078_999), [])
        self.assertEqual(result["capacity"], 9000)

    def test_no_missing_invalid_or_unknown_historical_rows_are_skipped(self):
        for entry in (None, "row", {}, dict(instance="old", ext_id_begin=1),
                      dict(instance="old", ext_id_begin=True, ext_id_end=10),
                      dict(instance="old", ext_id_begin=1, ext_id_end=10, account="private")):
            with self.subTest(entry=entry), self.assertRaises(ValueError):
                registry.validate_registration(self.proposal(), [entry])

    def test_incomplete_empty_or_unproven_scan_cannot_initialize_ledger(self):
        for changes in (dict(complete=False), dict(complete=1), dict(entries=[]),
                        dict(evidence_sha256="bad"), dict(extra="unknown")):
            with tempfile.TemporaryDirectory() as folder, self.subTest(changes=changes):
                with self.assertRaisesRegex(ValueError, "complete_historical_scan"):
                    self.call(folder, loader=lambda: self.snapshot(**changes))
                self.assertFalse((Path(folder) / "ledger.ndjson").exists())

    def test_exact_repeat_returns_original_receipt_without_appending(self):
        with tempfile.TemporaryDirectory() as folder:
            first = self.call(folder)
            ledger = Path(folder) / "ledger.ndjson"
            before = ledger.read_bytes()
            own = {key: self.proposal()[key] for key in registry.ENTRY_FIELDS}
            second = self.call(folder, loader=lambda: self.snapshot(entries=self.snapshot()["entries"] + [own]))
            self.assertFalse(second["appended"])
            self.assertEqual(second["receipt"], first["receipt"])
            self.assertEqual(ledger.read_bytes(), before)

    def test_repeat_does_not_hide_changed_historical_allocation(self):
        with tempfile.TemporaryDirectory() as folder:
            self.call(folder)
            collision = dict(instance="different_instance", ext_id_begin=2_000_070_000, ext_id_end=2_000_119_999)
            with self.assertRaisesRegex(ValueError, "overlap"):
                self.call(folder, loader=lambda: self.snapshot(entries=[collision]))

    def test_corrupt_truncated_or_unknown_record_refuses_without_rewriting(self):
        mutations = (lambda raw: raw[:-1], lambda raw: b"not json\n",
                     lambda raw: raw.replace(b'"ids_consumed":0', b'"ids_consumed":1'),
                     lambda raw: raw.replace(b'"armed":false', b'"armed":true,"armed":false'),
                     lambda raw: raw.replace(b'"schema":', b'"unknown":0,"schema":'))
        for mutate in mutations:
            with tempfile.TemporaryDirectory() as folder:
                self.call(folder)
                ledger = Path(folder) / "ledger.ndjson"
                damaged = mutate(ledger.read_bytes())
                ledger.write_bytes(damaged)
                with self.assertRaises(ValueError):
                    self.call(folder)
                self.assertEqual(ledger.read_bytes(), damaged)

    def test_multiple_records_are_append_only_chained_and_overlap_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            first = self.call(folder)
            before = (Path(folder) / "ledger.ndjson").read_bytes()
            second = self.call(folder, self.proposal(instance="next", ext_id_begin=2_000_120_000,
                                                      ext_id_end=2_000_169_999))
            self.assertEqual(second["receipt"]["previous_sha256"], first["receipt"]["record_sha256"])
            self.assertTrue((Path(folder) / "ledger.ndjson").read_bytes().startswith(before))
            with self.assertRaisesRegex(ValueError, "overlap"):
                self.call(folder, self.proposal(instance="third", ext_id_begin=2_000_119_999,
                                                ext_id_end=2_000_169_998))

    def test_concurrent_overlapping_requests_have_exactly_one_winner(self):
        with tempfile.TemporaryDirectory() as folder:
            barrier = threading.Barrier(2)
            results = []
            def contender(instance):
                barrier.wait(timeout=5)
                try:
                    results.append(self.call(folder, self.proposal(instance=instance)))
                except ValueError as error:
                    results.append(str(error))
            threads = [threading.Thread(target=contender, args=(name,)) for name in ("one", "two")]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=10)
                self.assertFalse(thread.is_alive())
            self.assertEqual(sum(isinstance(value, dict) for value in results), 1)
            self.assertIn("id_registry_overlap", results)
            self.assertEqual(len((Path(folder) / "ledger.ndjson").read_bytes().splitlines()), 1)

    def test_loader_runs_only_after_canonical_lock_is_held(self):
        import fcntl
        with tempfile.TemporaryDirectory() as folder:
            def load():
                fd = os.open(Path(folder) / "allocation.lock", os.O_RDWR)
                try:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                finally:
                    os.close(fd)
                return self.snapshot()
            self.call(folder, loader=load)

    def test_symlinks_or_nonprivate_existing_files_are_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / "target"
            target.write_text("preserve")
            target.chmod(0o600)
            (Path(folder) / "ledger.ndjson").symlink_to(target)
            with self.assertRaises(OSError):
                self.call(folder)
            self.assertEqual(target.read_text(), "preserve")
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / "ledger.ndjson"
            target.touch(mode=0o644)
            target.chmod(0o644)
            with self.assertRaisesRegex(ValueError, "private_regular"):
                self.call(folder)

    def test_absolute_distinct_paths_are_required_before_creating_files(self):
        with tempfile.TemporaryDirectory() as folder:
            for ledger, lock in ((Path("relative"), Path(folder) / "lock"),
                                 (Path(folder) / "ledger", Path(folder) / "." / "ledger")):
                with self.subTest(ledger=ledger), self.assertRaisesRegex(ValueError, "absolute_distinct"):
                    registry.register_range(ledger, self.proposal(), historical_loader=self.snapshot,
                                            lock_path=lock)
            self.assertEqual(list(Path(folder).iterdir()), [])

    def test_existing_private_global_lock_under_owned_775_parent_is_reused(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            root.chmod(0o775)
            lock = root / "canonical.lock"
            lock.touch(mode=0o600)
            before = lock.stat()
            result = registry.register_range(root / "private" / "ledger.ndjson", self.proposal(),
                                            historical_loader=self.snapshot, lock_path=lock)
            self.assertTrue(result["appended"])
            self.assertEqual(lock.stat().st_ino, before.st_ino)
            self.assertEqual(lock.read_bytes(), b"")
            self.assertEqual(root.stat().st_mode & 0o777, 0o775)
            self.assertEqual((root / "private").stat().st_mode & 0o777, 0o700)
            self.assertEqual((root / "private" / "ledger.ndjson").stat().st_mode & 0o777, 0o600)

    def test_missing_global_lock_is_never_created_in_775_parent(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            root.chmod(0o775)
            lock = root / "missing.lock"
            with self.assertRaisesRegex(ValueError, "parent_not_private"):
                registry.register_range(root / "private" / "ledger.ndjson", self.proposal(),
                                        historical_loader=self.snapshot, lock_path=lock)
            self.assertFalse(lock.exists())

    def test_existing_lock_in_world_writable_parent_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            root.chmod(0o777)
            lock = root / "canonical.lock"
            lock.touch(mode=0o600)
            with self.assertRaisesRegex(ValueError, "world_writable"):
                registry.register_range(root / "private" / "ledger.ndjson", self.proposal(),
                                        historical_loader=self.snapshot, lock_path=lock)

    def test_virgin_native_checkpoint_matches_capacity_parser_without_id_reservation(self):
        import ops
        proposal = self.proposal()
        begin, end = proposal["ext_id_begin"], proposal["ext_id_end"]
        checkpoint = registry.virgin_checkpoint(begin)
        self.assertEqual(checkpoint.split()[:7], [b"MOEXJ2", b"0", b"0", b"0",
                         b"2000070000", b"1", b"14695981039346656037"])
        report = registry.validate_virgin_checkpoint(checkpoint, begin, end)
        self.assertEqual(report["remaining"], 50_000)
        self.assertEqual(report["ids_consumed"], 0)
        with tempfile.TemporaryDirectory() as folder:
            state, assignment = Path(folder) / "new-state", Path(folder) / "new-assignment.json"
            with state.open("xb") as output:
                output.write(checkpoint)
            assignment.write_text(json.dumps(proposal))
            self.assertEqual(ops.capacity(state, assignment, proposal["instance"], begin, end), 50_000)
            self.assertEqual(state.read_bytes(), checkpoint)
            self.assertEqual({path.name for path in Path(folder).iterdir()},
                             {"new-state", "new-assignment.json"})

    def test_virgin_formatter_refuses_invalid_ids_and_nonvirgin_checkpoints(self):
        for value in (True, 0, -1, 2_147_483_648, "2000070000", 2_000_070_000.0):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "checkpoint_begin"):
                registry.virgin_checkpoint(value)
        proposal = self.proposal()
        begin, end = proposal["ext_id_begin"], proposal["ext_id_end"]
        checkpoint = registry.virgin_checkpoint(begin)
        for invalid in (checkpoint.replace(b"0 0 0", b"1 2 0"), checkpoint.replace(b" 1 ", b" 2 "),
                        registry.virgin_checkpoint(begin + 1000), checkpoint[:-2], checkpoint.decode()):
            with self.subTest(invalid=invalid), self.assertRaisesRegex(ValueError, "not_virgin"):
                registry.validate_virgin_checkpoint(invalid, begin, end)


if __name__ == "__main__":
    unittest.main()
