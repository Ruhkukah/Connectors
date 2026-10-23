"""Durable pre-Popen attempt budget shared by T0 supervisor and guardian.

Only normal trading-product starts are claimed here. Publisher-free diagnostics
must use their independent path. A consumed attempt is never rolled back, even
if Popen fails or no native startup block was written.
"""
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import stat
import time

from t0_id_registry import canonical, digest, _open_private
from t0_preparation import SOURCE, BINARY, DATE

SCHEMA = "moex.t0.start-attempt.v1"
ZERO = "0" * 64


def require(condition, code):
    if not condition:
        raise ValueError(code)


def _private(path, *, executable=False):
    path = Path(path)
    info = path.lstat()
    require(path.is_absolute() and stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
            and info.st_nlink == 1 and not info.st_mode & 0o022
            and (executable or stat.S_IMODE(info.st_mode) == 0o600),
            "t0_start_file_identity_or_permissions")
    return path


def _option(args, name):
    require(args.count(name) == 1 and args.index(name) + 1 < len(args),
            "t0_start_exact_argument_missing")
    return args[args.index(name) + 1]


def _checkpoint(path):
    tokens = path.read_bytes().split()
    require(len(tokens) == 8 and tokens[0] == b"MOEXJ2" and
            all(value.isdigit() for value in tokens[1:]), "t0_start_checkpoint_format")
    checksum = 14695981039346656037
    for byte in b" ".join(tokens[:7]):
        checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
    require(int(tokens[7]) == checksum, "t0_start_checkpoint_checksum")
    return tuple(map(int, tokens[1:7]))



BASELINE_SCHEMA = "moex.t0.clean-start-baseline.v1"
BASELINE_FILES = frozenset(("manifest.json", "configuration-private.json", "public.json",
                            "guardian-done.json", "native-start-attempts.ndjson"))


def _file_sha(path):
    return hashlib.sha256(_private(path).read_bytes()).hexdigest()


def _clean_terminal(public):
    closeout, after = public.get("closeout") or {}, public.get("after") or {}
    return (public.get("terminal") is True and closeout.get("cancel_confirmed") is True
            and closeout.get("flat") is True and type(public.get("product_exit")) is int
            and public["product_exit"] == 0 and type(after.get("client_count")) is int
            and after["client_count"] == 0 and public.get("no_guardian_failure") is not False
            and not any(public.get(key) or closeout.get(key) for key in
                        ("product_shutdown_error", "shutdown_error", "inventory_error")))


def _baseline(root, cfg, state, identity, begin, *, first):
    """Verify sealed predecessor evidence; never edit its files or checkpoint.

    The fresh sealer proves exact old process absence before producing the
    receipt. This pure file verifier checks that pinned proof, the real terminal
    receipts and the complete original start ledger. It never starts processes.
    """
    keys = ("start_baseline_receipt", "start_baseline_receipt_sha256")
    if not any(key in cfg for key in keys):
        return 0, None
    name, pin = (cfg.get(key) for key in keys)
    require(type(name) is str and name and Path(name).name == name
            and type(pin) is str and len(pin) == 64 and all(x in "0123456789abcdef" for x in pin),
            "t0_start_baseline_pin_missing")
    manifest = json.loads(_private(root / "manifest.json").read_bytes())
    require(manifest.get("source_git_sha") == SOURCE and manifest.get("binary_sha256") == BINARY
            and manifest.get("files", {}).get(name) == pin and _file_sha(root / name) == pin
            and manifest.get("files", {}).get("configuration-private.json")
                == _file_sha(root / "configuration-private.json")
            and json.loads(_private(root / "configuration-private.json").read_bytes()) == cfg,
            "t0_start_baseline_not_manifest_pinned")
    receipt = json.loads(_private(root / name).read_bytes())
    require(receipt.get("schema") == BASELINE_SCHEMA and receipt.get("target_venue") == "T0"
            and receipt.get("authorized_date") == cfg.get("authorized_date") == DATE
            and receipt.get("source_git_sha") == SOURCE and receipt.get("binary_sha256") == BINARY
            and receipt.get("instance") == cfg["instance"]
            and receipt.get("ext_id_range") == cfg["ext_id_range"]
            and receipt.get("assignment_sha256") == cfg["assignment_sha256"]
            and receipt.get("state_path_sha256") == identity["state_path_sha256"]
            and receipt.get("root_path_sha256") == hashlib.sha256(str(root).encode()).hexdigest()
            and type(cfg.get("account_mapping_sha256")) is str
            and len(cfg["account_mapping_sha256"]) == 64
            and receipt.get("account_mapping_sha256") == cfg["account_mapping_sha256"],
            "t0_start_baseline_scope_changed")
    require(receipt.get("old_exact_owners_stopped") is True
            and receipt.get("no_native_product_present") is True
            and type(receipt.get("baseline_blocks")) is int and receipt["baseline_blocks"] == 1
            and receipt.get("checkpoint_highwaters") == dict(ext=begin + 1000, user=1001),
            "t0_start_baseline_ownership_or_blocks_unproved")
    stamp = receipt.get("checked_utc_ns")
    require(type(stamp) is int and stamp > 0 and dt.datetime.fromtimestamp(
            stamp / 1e9, dt.timezone(dt.timedelta(hours=3))).date().isoformat() == DATE,
            "t0_start_baseline_calendar_unproved")
    if first:
        require(0 <= time.time_ns() - stamp <= 600_000_000_000,
                "t0_start_baseline_stale_or_future")
        require(_file_sha(state) == receipt.get("checkpoint_sha256"),
                "t0_start_baseline_checkpoint_changed")
    predecessor = Path(receipt.get("predecessor_root", ""))
    require(predecessor.is_absolute() and predecessor != root and not predecessor.is_symlink(),
            "t0_start_baseline_predecessor_path")
    oldinfo = predecessor.lstat()
    require(stat.S_ISDIR(oldinfo.st_mode) and oldinfo.st_uid == os.getuid()
            and stat.S_IMODE(oldinfo.st_mode) == 0o700,
            "t0_start_baseline_predecessor_permissions")
    files = receipt.get("predecessor_files")
    require(type(files) is dict and set(files) == BASELINE_FILES
            and all(type(value) is str and len(value) == 64 for value in files.values()),
            "t0_start_baseline_predecessor_files")
    require(all(_file_sha(predecessor / name) == value for name, value in files.items()),
            "t0_start_baseline_predecessor_changed")
    oldmanifest = json.loads(_private(predecessor / "manifest.json").read_bytes())
    oldcfg = json.loads(_private(predecessor / "configuration-private.json").read_bytes())
    require(oldmanifest.get("files", {}).get("configuration-private.json")
                == files["configuration-private.json"]
            and oldmanifest.get("source_git_sha") == SOURCE
            and oldmanifest.get("binary_sha256") == BINARY
            and all(oldcfg.get(key) == cfg.get(key) for key in
                    ("source_git_sha", "binary_sha256", "authorized_date", "instance", "ext_id_range",
                     "assignment_sha256", "state", "binary"))
            and oldcfg.get("target_venue") == "T0" and type(oldcfg.get("planned_starts")) is int
            and oldcfg["planned_starts"] == 2 and not any(key in oldcfg for key in keys),
            "t0_start_baseline_predecessor_scope_changed")
    mappings = []
    for values in (oldcfg, cfg):
        path = _private(values.get("account_mapping_reference", ""))
        require(_file_sha(path) == values.get("account_mapping_sha256"),
                "t0_start_baseline_mapping_pin_changed")
        mapping = json.loads(path.read_bytes())
        codes, login = mapping.get("client_codes"), mapping.get("login")
        require(type(codes) is list and len(codes) == 1 and type(codes[0]) is str
                and len(codes[0]) == 7 and codes[0].isalnum()
                and type(login) is str and bool(login), "t0_start_baseline_mapping_identity_missing")
        mappings.append((codes, login))
    require(mappings[0] == mappings[1], "t0_start_baseline_mapping_identity_changed")
    public = json.loads(_private(predecessor / "public.json").read_bytes())
    guardian = json.loads(_private(predecessor / "guardian-done.json").read_bytes())
    proof = guardian.get("independent_flatness") or {}
    require(_clean_terminal(public) and guardian.get("terminal") is True
            and guardian.get("state") == "SUPERVISOR_TERMINAL_VERIFIED"
            and guardian.get("supervisor_terminal_closeout_verified") is True
            and guardian.get("no_guardian_failure") is True and guardian.get("flat") is True
            and guardian.get("cancel_confirmed") is True
            and type(guardian.get("product_exit")) is int and guardian["product_exit"] == 0
            and type((guardian.get("after") or {}).get("client_count")) is int
            and guardian["after"]["client_count"] == 0
            and all(type(proof.get(key)) is int and proof[key] == value for key, value in
                    (("complete_replication_streams", 8), ("working_orders", 0),
                     ("nonzero_positions", 0), ("exit", 0)))
            and type((proof.get("after") or {}).get("client_count")) is int
            and proof["after"]["client_count"] == 0,
            "t0_start_baseline_clean_terminal_unproved")
    raw = _private(predecessor / "native-start-attempts.ndjson").read_bytes()
    require(raw.endswith(b"\n") and len(raw.splitlines()) == 1,
            "t0_start_baseline_parent_start_count")
    row = json.loads(raw)
    require(set(row) == {"schema", "sequence", "purpose", "utc_ns", "identity", "checkpoint_highwaters",
                         "previous_sha256", "receipt_sha256"}
            and row.get("schema") == SCHEMA and type(row.get("sequence")) is int and row["sequence"] == 1
            and row.get("purpose") == "initial" and row.get("previous_sha256") == ZERO
            and row.get("identity") == identity
            and row.get("checkpoint_highwaters") == dict(ext=begin, user=1)
            and type(row.get("utc_ns")) is int and 0 < row["utc_ns"] <= stamp
            and row.get("receipt_sha256") == digest({k: v for k, v in row.items() if k != "receipt_sha256"}),
            "t0_start_baseline_parent_ledger_invalid")
    return 1, pin


def claim_start(root, cfg, *, purpose, binary, state, args):
    """Claim attempt 1 (initial) or 2 (planned_restart/guardian_recovery).

    Call immediately before the product's sole Popen. ``cfg`` privately pins
    binary, state, assignment, assignment_sha256, native source/hash, instance,
    ext_id_range and planned_starts=2. All callers for this job must share root.
    A second call cannot use initial purpose. A failed second Popen consumes the
    same final attempt that a guardian recovery would otherwise use.
    """
    root = Path(root)
    info = root.lstat()
    require(root.is_absolute() and stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid()
            and stat.S_IMODE(info.st_mode) == 0o700, "t0_start_root_not_private_owned")
    require(cfg.get("source_git_sha") == SOURCE and cfg.get("binary_sha256") == BINARY
            and type(cfg.get("planned_starts")) is int and cfg["planned_starts"] == 2,
            "t0_start_native_or_budget_pin")
    require(purpose in ("initial", "planned_restart", "guardian_recovery"), "t0_start_purpose")
    binary, state = _private(binary, executable=True), _private(state)
    assignment = _private(cfg["assignment"])
    require(str(binary) == cfg["binary"] and str(state) == cfg["state"] and
            hashlib.sha256(binary.read_bytes()).hexdigest() == BINARY and
            hashlib.sha256(assignment.read_bytes()).hexdigest() == cfg["assignment_sha256"],
            "t0_start_registered_identity_changed")
    require(type(args) is list and all(type(value) is str for value in args) and
            args[:3] == [str(binary), "plaza2", "run"] and args.count("--allow-orders") == 1
            and _option(args, "--environment") == "test" and
            _option(args, "--state") == str(state) and
            _option(args, "--ext-id-range") == cfg["ext_id_range"] and
            _option(args, "--instance-id") == cfg["instance"], "t0_start_scope_or_arguments")
    try:
        begin, end = map(int, cfg["ext_id_range"].split(":"))
        allocation = json.loads(assignment.read_bytes())
    except (ValueError, TypeError):
        raise ValueError("t0_start_assignment_format") from None
    require(all(type(allocation.get(key)) is int for key in ("ext_id_begin", "ext_id_end")) and
            allocation.get("instance") == cfg["instance"] and allocation["ext_id_begin"] == begin and
            allocation["ext_id_end"] == end and 1 <= begin <= end <= 2_147_483_647 and
            end - begin + 1 >= 9000, "t0_start_assignment_changed")
    identity = dict(source_git_sha=SOURCE, binary_sha256=BINARY,
                    state_path_sha256=hashlib.sha256(str(state).encode()).hexdigest(),
                    assignment_sha256=cfg["assignment_sha256"], instance=cfg["instance"],
                    ext_id_range=cfg["ext_id_range"], planned_starts=2)
    lock_fd = _open_private(root / "native-start-budget.lock")
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        ledger_fd = _open_private(root / "native-start-attempts.ndjson")
        try:
            size = os.fstat(ledger_fd).st_size
            require(size < 16384, "t0_start_ledger_oversize")
            raw = os.read(ledger_fd, size + 1)
            require(len(raw) == size and (not raw or raw.endswith(b"\n")), "t0_start_ledger_truncated")
            previous = ZERO
            rows = raw.splitlines()
            baseline_blocks, baseline_pin = _baseline(root, cfg, state, identity, begin, first=not rows)
            if baseline_pin is not None:
                identity["start_baseline_receipt_sha256"] = baseline_pin
            for sequence, line in enumerate(rows, 1):
                row = json.loads(line)
                require(set(row) == {"schema", "sequence", "purpose", "utc_ns", "identity",
                                     "checkpoint_highwaters", "previous_sha256", "receipt_sha256"}
                        and row["schema"] == SCHEMA and type(row["sequence"]) is int
                        and row["sequence"] == sequence and row["identity"] == identity
                        and row["previous_sha256"] == previous
                        and row["purpose"] in (("initial",) if sequence == 1 else
                                                ("planned_restart", "guardian_recovery"))
                        and digest({key: value for key, value in row.items() if key != "receipt_sha256"})
                            == row["receipt_sha256"], "t0_start_ledger_invalid")
                previous = row["receipt_sha256"]
            require(len(rows) < 2, "t0_start_two_attempt_budget_exhausted")
            require((not rows and purpose == "initial") or
                    (len(rows) == 1 and purpose in ("planned_restart", "guardian_recovery")),
                    "t0_start_retry_or_purpose_refused")
            device, inode, offset, ext, user, boundary = _checkpoint(state)
            expected_blocks = baseline_blocks + len(rows)
            require(ext == begin + expected_blocks * 1000 and user == 1 + expected_blocks * 1000,
                    "t0_start_native_block_continuity_unproved")
            if not rows and not baseline_blocks:
                require((device, inode, offset, boundary) == (0, 0, 0, 14695981039346656037),
                        "t0_start_initial_checkpoint_not_virgin")
            remaining = end - ext + 1
            require(remaining >= (9000 if not rows else 8000), "t0_start_identifier_capacity")
            row = dict(schema=SCHEMA, sequence=len(rows) + 1, purpose=purpose, utc_ns=time.time_ns(),
                       identity=identity, checkpoint_highwaters=dict(ext=ext, user=user),
                       previous_sha256=previous)
            row["receipt_sha256"] = digest(row)
            encoded = canonical(row) + b"\n"
            require(os.write(ledger_fd, encoded) == len(encoded), "t0_start_attempt_write_uncertain")
            os.fsync(ledger_fd)
            directory_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
            return dict(attempt=row["sequence"], receipt_sha256=row["receipt_sha256"],
                        remaining_ids_before_start=remaining, purpose=purpose,
                        native_process_started=False, orders_sent=False)
        finally:
            os.close(ledger_fd)
    finally:
        fcntl.flock(lock_fd, fcntl.LOCK_UN)
        os.close(lock_fd)
