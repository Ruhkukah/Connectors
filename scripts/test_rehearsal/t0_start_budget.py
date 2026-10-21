"""Durable pre-Popen attempt budget shared by T0 supervisor and guardian.

Only normal trading-product starts are claimed here. Publisher-free diagnostics
must use their independent path. A consumed attempt is never rolled back, even
if Popen fails or no native startup block was written.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import stat
import time

from t0_id_registry import canonical, digest, _open_private
from t0_preparation import SOURCE, BINARY

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
            expected_blocks = len(rows)
            require(ext == begin + expected_blocks * 1000 and user == 1 + expected_blocks * 1000,
                    "t0_start_native_block_continuity_unproved")
            if not rows:
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
