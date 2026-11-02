"""Load an explicitly selected protected adapter; never supply live defaults."""
from pathlib import Path
import hashlib
import importlib.util
import os
import re


def load(environ=None):
    env = os.environ if environ is None else environ
    location = env.get("MOEX_REHEARSAL_ADAPTER", "")
    expected = env.get("MOEX_REHEARSAL_ADAPTER_SHA256", "")
    path = Path(location)
    if not location or not path.is_absolute() or not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise RuntimeError("protected_adapter_and_hash_required")
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise RuntimeError("protected_adapter_hash_changed")
    spec = importlib.util.spec_from_file_location("protected_rehearsal_adapter", path)
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)
    cfg = dict(adapter.settings())
    work = Path(cfg.pop("work_directory"))
    repository = Path(__file__).resolve().parents[2]
    if not work.is_absolute() or not work.is_dir() or work.resolve().is_relative_to(repository):
        raise RuntimeError("protected_existing_work_directory_required")
    if not re.fullmatch(r"[0-9a-f]{64}", cfg.get("binary_sha256", "")):
        raise RuntimeError("product_binary_hash_required")
    match = re.fullmatch(r"([0-9]+):([0-9]+)", cfg.get("ext_id_range", ""))
    if not match or not 0 < int(match[1]) <= int(match[2]) < 2**31:
        raise RuntimeError("assigned_ext_id_range_required")
    for method in ("setup", "client_isolation", "prove_fill_coverage", "reconstruct_orders"):
        if not callable(getattr(adapter, method, None)):
            raise RuntimeError("protected_adapter_contract_incomplete")
    return adapter, work, cfg
