"""Append-only T0 ID registration; no checkpoint, native process or arming API.

The caller supplies a fresh, complete historical allocation scan while holding
the same canonical allocation lock used by other ID writers. Registration does
not reserve a native startup block and consumes no IDs.
"""
from __future__ import annotations

import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import time


SCHEMA = "moex.t0.id-registration.v1"
ZERO = "0" * 64
FNV_EMPTY = 14695981039346656037
PROPOSAL_FIELDS = {"instance", "ext_id_begin", "ext_id_end", "venue", "purpose"}
ENTRY_FIELDS = {"instance", "ext_id_begin", "ext_id_end"}
RECORD_FIELDS = {"schema", "sequence", "previous_sha256", "registration", "authority",
                 "registered_utc_ns", "ids_consumed", "armed", "allow_orders", "record_sha256"}


def require(condition, code):
    if not condition:
        raise ValueError(code)


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def virgin_checkpoint(begin):
    """Return MOEXJ2 bytes for a newly registered, never-started namespace only.

    Mirrors EventJournal's checkpoint_record/text_hash fields: device, inode,
    offset, next ext ID, next user ID and empty-boundary FNV hash. The caller
    must create an absent file exclusively in a fresh namespace directory;
    these bytes must never replace or reset an existing identity checkpoint.
    This pure formatter does not write files or reserve a startup ID block.
    """
    require(type(begin) is int and 1 <= begin <= 2_147_483_647,
            "id_registry_checkpoint_begin")
    record = f"MOEXJ2 0 0 0 {begin} 1 {FNV_EMPTY}".encode("ascii")
    checksum = FNV_EMPTY
    for byte in record:
        checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return record + b" " + str(checksum).encode("ascii") + b"\n"


def validate_virgin_checkpoint(checkpoint, begin, end):
    require(type(end) is int and type(begin) is int and
            1 <= begin <= end <= 2_147_483_647, "id_registry_numeric_range")
    require(type(checkpoint) is bytes and checkpoint == virgin_checkpoint(begin),
            "id_registry_checkpoint_not_virgin")
    require(end - begin + 1 >= 9_000, "id_registry_two_start_capacity")
    return dict(virgin=True, remaining=end - begin + 1, ids_consumed=0,
                next_user_id=1, armed=False, can_trade=False)


def _sha(value):
    return type(value) is str and re.fullmatch(r"[a-f0-9]{64}", value) is not None


def _entry(row):
    require(type(row) is dict and set(row) == ENTRY_FIELDS, "id_registry_historical_fields")
    require(type(row["instance"]) is str and
            re.fullmatch(r"[A-Za-z0-9_.-]{1,100}", row["instance"]) is not None,
            "id_registry_instance")
    begin, end = row["ext_id_begin"], row["ext_id_end"]
    require(type(begin) is int and type(end) is int and 1 <= begin <= end <= 2_147_483_647,
            "id_registry_numeric_range")
    return begin, end


def _proposal(proposal):
    require(type(proposal) is dict and set(proposal) == PROPOSAL_FIELDS,
            "id_registry_proposal_fields")
    require(proposal["venue"] == "T0" and proposal["purpose"] == "qualification",
            "id_registry_scope")
    begin, end = _entry({key: proposal[key] for key in ENTRY_FIELDS})
    require(end - begin + 1 >= 9_000, "id_registry_two_start_capacity")
    return begin, end


def validate_registration(proposal, existing_entries):
    """Refuse overlap, instance reuse or malformed historical rows, across venues.

    Historical extensions may overlap each other. Every supplied row is still
    checked against the proposed inclusive interval; nothing is silently filtered.
    This pure consistency check alone is not allocation or execution authority.
    """
    begin, end = _proposal(proposal)
    require(type(existing_entries) is list, "id_registry_historical_list")
    for entry in existing_entries:
        other_begin, other_end = _entry(entry)
        require(entry["instance"] != proposal["instance"], "id_registry_instance_reuse")
        require(end < other_begin or begin > other_end, "id_registry_overlap")
    return dict(consistent=True, capacity=end - begin + 1, ids_consumed=0,
                armed=False, can_trade=False, historical_ranges_checked=len(existing_entries))


def _private_parent(path):
    parent = path.parent
    parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = parent.stat()
    require(info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o700,
            "id_registry_parent_not_private_owned")


def _open_private(path, *, create=True):
    flags = os.O_RDWR | os.O_APPEND | os.O_NOFOLLOW
    fd = os.open(path, flags | (os.O_CREAT if create else 0), 0o600)
    info = os.fstat(fd)
    if not (stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and
            stat.S_IMODE(info.st_mode) == 0o600 and info.st_nlink == 1):
        os.close(fd)
        raise ValueError("id_registry_file_not_private_regular_owned")
    return fd


def _open_allocation_lock(path):
    if path.exists() or path.is_symlink():
        # Reuse the canonical allocation lock even when the containing owned
        # qualification directory is group-accessible. Never replace or create
        # a lock in that directory; O_NOFOLLOW and the fd checks still apply.
        info = path.parent.stat()
        require(info.st_uid == os.getuid() and not info.st_mode & stat.S_IWOTH,
                "id_registry_lock_parent_not_owned_or_world_writable")
        return _open_private(path, create=False)
    _private_parent(path)
    return _open_private(path)


def _records(fd):
    size = os.fstat(fd).st_size
    require(size <= 4 * 1024 * 1024, "id_registry_oversize")
    os.lseek(fd, 0, os.SEEK_SET)
    raw = os.read(fd, size + 1)
    require(len(raw) == size and (not raw or raw.endswith(b"\n")), "id_registry_truncated")
    records, previous, registrations = [], ZERO, []
    for sequence, line in enumerate(raw.splitlines(), 1):
        def unique_fields(pairs):
            row = {}
            for key, value in pairs:
                require(key not in row, "id_registry_duplicate_json_field")
                row[key] = value
            return row
        try:
            record = json.loads(line, object_pairs_hook=unique_fields)
        except (ValueError, UnicodeDecodeError):
            raise ValueError("id_registry_invalid_json") from None
        require(type(record) is dict and set(record) == RECORD_FIELDS, "id_registry_record_fields")
        require(record["schema"] == SCHEMA and type(record["sequence"]) is int and
                record["sequence"] == sequence and record["previous_sha256"] == previous,
                "id_registry_chain")
        require(type(record["registered_utc_ns"]) is int and record["registered_utc_ns"] > 0 and
                type(record["ids_consumed"]) is int and record["ids_consumed"] == 0 and
                record["armed"] is False and record["allow_orders"] is False,
                "id_registry_execution_disabled")
        authority = record["authority"]
        require(type(authority) is dict and set(authority) == {"evidence_sha256", "entries_sha256", "range_count"}
                and _sha(authority["evidence_sha256"]) and _sha(authority["entries_sha256"])
                and type(authority["range_count"]) is int and authority["range_count"] > 0,
                "id_registry_authority")
        payload = {key: value for key, value in record.items() if key != "record_sha256"}
        require(_sha(record["record_sha256"]) and digest(payload) == record["record_sha256"],
                "id_registry_checksum")
        validate_registration(record["registration"], registrations)
        registrations.append({key: record["registration"][key] for key in ENTRY_FIELDS})
        previous = record["record_sha256"]
        records.append(record)
    return records


def register_range(registry_path, proposal, *, historical_loader, lock_path):
    """Register once under the canonical allocation lock, returning a receipt.

    ``historical_loader`` executes inside the lock and returns exactly
    ``{complete: True, evidence_sha256: <digest>, entries: <normalized list>}``.
    The scan must cover all historical assignments, including other venues.
    Empty/incomplete scans, malformed ledgers and concurrent overlaps refuse.
    Exact repeats return the original receipt without another append or IDs.
    """
    _proposal(proposal)
    proposal = dict(proposal)
    registry_path, lock_path = Path(registry_path), Path(lock_path)
    require(registry_path.is_absolute() and lock_path.is_absolute() and
            registry_path.resolve() != lock_path.resolve(), "id_registry_absolute_distinct_paths")
    _private_parent(registry_path)
    lock_fd = _open_allocation_lock(lock_path)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        snapshot = historical_loader()
        require(type(snapshot) is dict and set(snapshot) == {"complete", "evidence_sha256", "entries"}
                and snapshot["complete"] is True and _sha(snapshot["evidence_sha256"])
                and type(snapshot["entries"]) is list and len(snapshot["entries"]) > 0,
                "id_registry_complete_historical_scan_required")
        # Validate every historical row even when this is an idempotent repeat.
        for entry in snapshot["entries"]:
            _entry(entry)
        registry_fd = _open_private(registry_path)
        try:
            records = _records(registry_fd)
            own = {key: proposal[key] for key in ENTRY_FIELDS}
            matching = next((row for row in records if row["registration"] == proposal), None)
            historical = snapshot["entries"]
            if matching:
                validate_registration(proposal, [row for row in historical if row != own])
                return dict(receipt=matching, appended=False, ids_consumed=0, armed=False, can_trade=False)
            existing = historical + [{key: row["registration"][key] for key in ENTRY_FIELDS} for row in records]
            validate_registration(proposal, existing)
            record = dict(schema=SCHEMA, sequence=len(records) + 1,
                          previous_sha256=records[-1]["record_sha256"] if records else ZERO,
                          registration=dict(proposal),
                          authority=dict(evidence_sha256=snapshot["evidence_sha256"],
                                         entries_sha256=digest(historical), range_count=len(historical)),
                          registered_utc_ns=time.time_ns(), ids_consumed=0, armed=False, allow_orders=False)
            record["record_sha256"] = digest(record)
            encoded = canonical(record) + b"\n"
            require(os.write(registry_fd, encoded) == len(encoded), "id_registry_append_uncertain")
            os.fsync(registry_fd)
            directory_fd = os.open(registry_path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
            return dict(receipt=record, appended=True, ids_consumed=0, armed=False, can_trade=False)
        finally:
            os.close(registry_fd)
    finally:
        fcntl.flock(lock_fd, fcntl.LOCK_UN)
        os.close(lock_fd)
