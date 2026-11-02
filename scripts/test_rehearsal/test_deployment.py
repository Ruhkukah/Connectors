"""Hermetic adapter-selection checks; no router, process, CGate or network."""
from pathlib import Path
import hashlib
import tempfile
import unittest

from deployment import load


class AdapterBoundary(unittest.TestCase):
    def test_no_default_deployment(self):
        with self.assertRaisesRegex(RuntimeError, "protected_adapter_and_hash_required"):
            load({})

    def test_hash_mismatch_prevents_execution(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "adapter.py"
            path.write_text("raise AssertionError('adapter_must_not_execute')\n")
            with self.assertRaisesRegex(RuntimeError, "protected_adapter_hash_changed"):
                load({"MOEX_REHEARSAL_ADAPTER": str(path), "MOEX_REHEARSAL_ADAPTER_SHA256": "0" * 64})

    def fixture(self, folder, work, id_range="1000:30999"):
        path = Path(folder) / "adapter.py"
        path.write_text(
            "def settings():\n"
            f" return dict(work_directory={str(work)!r}, binary_sha256={'a'*64!r}, ext_id_range={id_range!r})\n"
            "def setup(*args): raise RuntimeError('not_live')\n"
            "def client_isolation(*args): raise RuntimeError('not_live')\n"
            "def prove_fill_coverage(*args): raise RuntimeError('not_live')\n"
            "def reconstruct_orders(*args): raise RuntimeError('not_live')\n"
        )
        return {"MOEX_REHEARSAL_ADAPTER": str(path), "MOEX_REHEARSAL_ADAPTER_SHA256": hashlib.sha256(path.read_bytes()).hexdigest()}

    def test_repository_cannot_be_work_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(RuntimeError, "protected_existing_work_directory_required"):
                load(self.fixture(folder, Path(__file__).resolve().parent))

    def test_range_must_be_assigned_and_bounded(self):
        with tempfile.TemporaryDirectory() as folder:
            for value in ("", "0:2", "3:2", "1:2147483648"):
                with self.subTest(value=value):
                    with self.assertRaisesRegex(RuntimeError, "assigned_ext_id_range_required"):
                        load(self.fixture(folder, folder, value))

    def test_explicit_test_adapter_loads_without_commands(self):
        with tempfile.TemporaryDirectory() as folder:
            adapter, work, cfg = load(self.fixture(folder, folder))
            self.assertEqual(work, Path(folder))
            self.assertEqual(cfg["ext_id_range"], "1000:30999")
            with self.assertRaisesRegex(RuntimeError, "not_live"):
                adapter.setup(work, cfg)


if __name__ == "__main__":
    unittest.main()
