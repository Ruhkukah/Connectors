#!/usr/bin/env python3
"""Build a small, offline, redacted MOEX T1 access-denial support bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from datetime import datetime, timedelta, timezone
from pathlib import Path


LISTENER = "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL"
EXACT_ERRORS = ("REPL:ACCESS_DENIED", "40969 / 0xA009", "Open request rejected by server")
PRIVATE_KEYS = {
    "password", "pass", "passwd", "token", "secret", "credential", "connection", "uri", "env", "login", "user",
    "username", "firm", "local_pass", "app_name",
}
PRIVATE_URL = re.compile(r"p2(?:tcp|lrpcq|sys)://[^\s\"']+", re.IGNORECASE)
PRIVATE_VALUE = re.compile(
    r"(?i)(\b(?:password|passwd|pass|token|secret|credential|connection|uri|env|"
    r"login|user|username|firm|local_pass|app_name)\s*[=:]\s*)(\"[^\"]*\"|'[^']*'|[^\s,;]+)"
)
TIMESTAMP_KEYS = ("timestamp", "timestamp_ns", "time", "time_ns", "diagnostic_monotonic_ns")
MOSCOW = timezone(timedelta(hours=3), "MSK")


def sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def redact(value: object) -> object:
    if isinstance(value, dict):
        return {key: redact(item) for key, item in value.items() if key.lower() not in PRIVATE_KEYS}
    if isinstance(value, list):
        return [redact(item) for item in value]
    if isinstance(value, str):
        return PRIVATE_VALUE.sub(r"\1[REDACTED]", PRIVATE_URL.sub("[REDACTED_PRIVATE_URI]", value))
    return value


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def trace_digest(evidence: Path, manifest: dict) -> str:
    trace = evidence / "capture.bin"
    sidecar = evidence / "capture.bin.sha256"
    digest = sha(trace) if trace.is_file() else ""
    declared = sidecar.read_text().strip() if sidecar.is_file() else manifest.get("source_sha256", "")
    if digest and declared and digest != declared:
        raise SystemExit("capture SHA-256 mismatch")
    return digest or declared


def timestamp(row: dict) -> object:
    for key in TIMESTAMP_KEYS:
        if key in row:
            return row[key]
    return None


def compact(row: dict, keys: tuple[str, ...]) -> dict:
    return redact({key: row[key] for key in keys if key in row})


def read_controls(evidence: Path) -> tuple[list[dict], list[dict], list[object]]:
    controls = evidence / "derived" / "controls.jsonl"
    if not controls.is_file():
        return [], [], []
    diagnostics: list[dict] = []
    states: list[dict] = []
    timestamps: list[object] = []
    legacy: dict[tuple[object, object], dict] = {}
    legacy_transitions: list[dict] = []
    with controls.open() as source:
        for line in source:
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not isinstance(row, dict):
                continue
            text = " ".join(str(row.get(key, "")) for key in ("sdk_error_text", "text", "outcome"))
            relevant = any(token in text for token in ("REPL:ACCESS_DENIED", "40969", "0xA009", "Open request rejected"))
            if row.get("sdk_error_text") or (relevant and (row.get("operation") == "listener_state" or row.get("kind") == 6)):
                item = compact(
                    row,
                    ("listener", "epoch", "poll", "operation", "code", "text", "sdk_error_text", "diagnostic_monotonic_ns",
                     "timestamp", "timestamp_ns", "time", "time_ns"),
                )
                diagnostics.append(item)
                value = timestamp(row)
                if value is not None and value not in timestamps:
                    timestamps.append(value)
            event = row.get("event")
            if event in ("listener_state_transition", "listener_state_summary"):
                states.append(compact(row, ("listener", "epoch", "poll", "event", "transition", "state", "previous_state",
                                             "first_observation_ordinal", "last_observation_ordinal",
                                             "first_observation_monotonic_ns", "last_observation_monotonic_ns", "observations",
                                             "unchanged_polls", "state_error", "first_sdk_error_code", "first_sdk_diagnostic",
                                             "outcome")))
            elif "listener_state" in row or row.get("outcome") == "STATE_ERROR":
                key = (row.get("listener"), row.get("epoch"))
                state = row.get("listener_state", 1 if row.get("outcome") == "STATE_ERROR" else None)
                if state is not None:
                    item = legacy.get(key)
                    new_state = item is None or item["state"] != state
                    if item is None:
                        item = {
                            "listener": row.get("listener"), "epoch": row.get("epoch"), "state": state,
                            "previous_state": None, "first_observation_ordinal": row.get("poll", 0),
                            "last_observation_ordinal": row.get("poll", 0),
                            "first_observation_monotonic_ns": row.get("monotonic_ns"),
                            "last_observation_monotonic_ns": row.get("monotonic_ns"),
                            "observations": 0, "unchanged_polls": 0, "state_error": state == 1,
                            "outcome": "STATE_ERROR" if state == 1 else None,
                        }
                        legacy[key] = item
                    if new_state:
                        legacy_transitions.append({
                            "listener": row.get("listener"), "epoch": row.get("epoch"),
                            "event": "listener_state_transition", "transition": True, "state": state,
                            "previous_state": item["state"] if item["observations"] else None,
                            "first_observation_ordinal": row.get("poll", 0),
                            "first_observation_monotonic_ns": row.get("monotonic_ns"),
                            "state_error": state == 1,
                            "outcome": "STATE_ERROR" if state == 1 else None,
                        })
                    if item["state"] != state:
                        item["previous_state"] = item["state"]
                        item["state"] = state
                        item["state_error"] = state == 1
                        item["outcome"] = "STATE_ERROR" if state == 1 else None
                        item["first_observation_ordinal"] = row.get("poll", 0)
                        item["first_observation_monotonic_ns"] = row.get("monotonic_ns")
                        item["observations"] = 0
                        item["unchanged_polls"] = 0
                    item["observations"] += 1
                    if item["observations"] > 1:
                        item["unchanged_polls"] += 1
                    item["last_observation_ordinal"] = row.get("poll", 0)
                    item["last_observation_monotonic_ns"] = row.get("monotonic_ns")
            if len(timestamps) >= 2:
                timestamps = timestamps[:2]
    states.extend(redact(item) | {"event": "listener_state_summary"} for item in legacy.values())
    states[0:0] = [redact(item) for item in legacy_transitions]
    return diagnostics, states, timestamps


def report_diagnostic(evidence: Path) -> tuple[list[dict], list[object]]:
    report = evidence / "REPORT.md"
    if not report.is_file():
        return [], []
    text = report.read_text(errors="replace")
    if not any(token in text for token in EXACT_ERRORS):
        return [], []
    return [{"sdk_error_text": redact(text[text.find("p2err") : text.find("p2err") + 120]).strip()}], []


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def build(evidence: Path, output: Path, metadata_path: Path | None) -> None:
    if not evidence.is_dir():
        raise SystemExit("evidence directory is unavailable")
    if output.exists():
        raise SystemExit("output directory must be new")
    manifest = load_json(evidence / "manifest.json")
    environment = load_json(evidence / "derived" / "environment.json")
    metrics = load_json(evidence / "metrics.json")
    analysis = load_json(evidence / "derived" / "analysis.json")
    trace = trace_digest(evidence, manifest)
    diagnostics, states, timestamps = read_controls(evidence)
    if not diagnostics:
        diagnostics, timestamps = report_diagnostic(evidence)
    runtime = {
        "version": environment.get("runtime_version", "UNKNOWN"),
        "sha256": environment.get("runtime_sha256", "UNKNOWN"),
    }
    private = {"login": "[NOT_SUPPLIED]", "firm_section": "[NOT_SUPPLIED]", "login_class": "UNKNOWN"}
    if metadata_path is not None:
        supplied = load_json(metadata_path)
        for key in private:
            if key in supplied:
                private[key] = str(supplied[key])
    now = datetime.now(timezone.utc)
    summary = {
        "outcome": "BLOCKED_REGULAR_REPLICATION_ACCESS_DENIED",
        "external_gate": "WAITING_FOR_MOEX_FULL_ORDERS_LOG_ACCESS_CONFIRMATION",
        "polygon": "T1",
        "protocol": "Plaza II",
        "listener": LISTENER,
        "trace_sha256": trace,
        "connector_sha": environment.get("connector_commit", "UNKNOWN"),
        "runtime": runtime,
        "generated_at_utc": now.isoformat().replace("+00:00", "Z"),
        "generated_at_moscow": now.astimezone(MOSCOW).isoformat(),
        "first_error_timestamps": timestamps[:2],
        "exact_errors": list(EXACT_ERRORS),
        "capture_loss": {
            key: metrics.get(key, 0)
            for key in ("callbacks_received", "capture_overflow", "write_failures", "descriptor_failures", "first_lost_ordinal",
                        "first_lost_listener", "redaction_failures")
        },
        "oracle_outcome": analysis.get("oracle", {}).get("equivalence", "NOT_OBSERVED"),
        "private_support_metadata": private,
    }
    output.mkdir(mode=0o700)
    write_json(output / "support_summary.json", summary)
    write_json(output / "diagnostics.json", diagnostics[:64])
    write_json(output / "state_transitions.json", states[:64])
    lines = [
        "MOEX Plaza II T1 support summary",
        f"outcome: {summary['outcome']}",
        f"external_gate: {summary['external_gate']}",
        "polygon: T1",
        "protocol: Plaza II",
        f"listener: {LISTENER}",
        f"trace_sha256: {trace}",
        f"connector_sha: {summary['connector_sha']}",
        f"runtime: {runtime['version']} ({runtime['sha256']})",
        f"generated_at_utc: {summary['generated_at_utc']}",
        f"generated_at_moscow: {summary['generated_at_moscow']}",
        "errors: REPL:ACCESS_DENIED; 40969 / 0xA009; Open request rejected by server",
        f"first_error_timestamps: {json.dumps(timestamps[:2])}",
        f"capture_loss: {json.dumps(summary['capture_loss'], sort_keys=True)}",
        "credentials/raw capture: excluded",
    ]
    (output / "support_summary.txt").write_text("\n".join(lines) + "\n")
    files = {}
    for path in sorted(output.iterdir()):
        if path.name == "manifest.json":
            continue
        path.chmod(0o600)
        files[path.name] = sha(path)
    write_json(output / "manifest.json", {"format": 1, "files": files, "raw_capture_included": False})
    (output / "manifest.json").chmod(0o600)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--metadata-json", type=Path)
    args = parser.parse_args()
    build(args.evidence, args.output, args.metadata_json)


if __name__ == "__main__":
    main()
