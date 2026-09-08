#!/usr/bin/env python3
"""Exercise support-bundle selection and credential/private-URI redaction."""

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/plaza2_c2_support_bundle.py"


with tempfile.TemporaryDirectory(prefix="plaza2_c2_support_") as tmp:
    root = Path(tmp)
    evidence = root / "evidence"
    derived = evidence / "derived"
    derived.mkdir(parents=True)
    trace = evidence / "capture.bin"
    trace.write_bytes(b"immutable-capture")
    digest = hashlib.sha256(trace.read_bytes()).hexdigest()
    (evidence / "capture.bin.sha256").write_text(digest + "\n")
    (evidence / "manifest.json").write_text(json.dumps({"source_sha256": digest}))
    (evidence / "metrics.json").write_text(json.dumps({"callbacks_received": 0, "capture_overflow": 0,
                                                        "write_failures": 0, "descriptor_failures": 0,
                                                        "first_lost_ordinal": 0, "first_lost_listener": 0,
                                                        "redaction_failures": 0}))
    (derived / "environment.json").write_text(json.dumps({
        "connector_commit": "0008f631b7e4df5f12efade5c8b3d796a1bc8407",
        "runtime_version": "6.102.0",
        "runtime_sha256": "runtime-hash",
        "listeners": [{"url": "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL"}],
    }))
    (derived / "analysis.json").write_text(json.dumps({"oracle": {"equivalence": "NOT_OBSERVED"}}))
    (derived / "controls.jsonl").write_text(
        json.dumps({"kind": 6, "listener": 0, "epoch": 1,
                    "sdk_error_text": "2026-09-08T04:01:00Z p2err 40969=0xA009 REPL:ACCESS_DENIED; "
                                       "Open request rejected by server; p2tcp://private.example:4001 "
                                       "password=Secret-Capture-Credential-123",
                    "diagnostic_monotonic_ns": 101}) + "\n"
        + json.dumps({"kind": 6, "listener": 0, "epoch": 1,
                      "sdk_error_text": "2026-09-08T04:01:01Z p2err 40969=0xA009 REPL:ACCESS_DENIED; "
                                         "Open request rejected by server",
                      "diagnostic_monotonic_ns": 202}) + "\n"
        + json.dumps({"listener": 0, "epoch": 1, "event": "listener_state_transition", "transition": True,
                      "state": 1, "previous_state": None, "first_observation_ordinal": 1,
                      "first_observation_monotonic_ns": 99, "state_error": True, "outcome": "STATE_ERROR"}) + "\n"
        + json.dumps({"listener": 0, "epoch": 1, "event": "listener_state_summary", "transition": False,
                      "state": 1, "observations": 100001, "unchanged_polls": 100000, "state_error": True}) + "\n"
    )
    output = root / "bundle"
    subprocess.run([sys.executable, str(TOOL), str(evidence), "--output", str(output)], check=True)
    summary = json.loads((output / "support_summary.json").read_text())
    assert summary["first_error_timestamps"] == [101, 202]
    assert summary["listener"] == "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL"
    assert summary["exact_errors"] == ["REPL:ACCESS_DENIED", "40969 / 0xA009", "Open request rejected by server"]
    assert not (output / "capture.bin").exists()
    for path in output.iterdir():
        content = path.read_text(errors="replace")
        assert "Secret-Capture-Credential-123" not in content
        assert "p2tcp://" not in content
        assert "REPL:ACCESS_DENIED" in content or path.name in {"manifest.json", "state_transitions.json"}
    print("SUPPORT BUNDLE PASS: exact denial, first two timestamps, private URI/credential redaction, raw exclusion")
