#!/usr/bin/env python3
"""Emit the future one-shot Plaza II entitlement probe plan without opening CGate."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCHEME_ROOT = "QUALIFIED_SCHEME_DIR"
MODES = (
    {
        "name": "ORDLOG_ONLY",
        "listener": f"p2repl://FORTS_ORDLOG_REPL;scheme=|FILE|{SCHEME_ROOT}/ordLog_trades.ini|CustReplScheme",
        "scheme": "ordLog_trades.ini",
        "source_docs": ("docs/plaza2/RAW_ORDLOG_C1.md", "tests/plaza2_cgate/plaza2_aggr20_md_validation_test.cpp"),
    },
    {
        "name": "ORDBOOK_ONLY",
        "listener": f"p2repl://FORTS_ORDBOOK_REPL;scheme=|FILE|{SCHEME_ROOT}/ordbook.ini|CustReplScheme",
        "scheme": "ordbook.ini",
        "source_docs": ("docs/plaza2/PUBLIC_L3_MUTATION_CONTRACT_9_9.md", "tests/plaza2_cgate/plaza2_aggr20_md_validation_test.cpp"),
    },
    {
        "name": "COMPOSITE_P2ORDBOOK",
        "listener": (
            "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;name=entitlement_probe;"
            f"online.scheme=|FILE|{SCHEME_ROOT}/ordLog_trades.ini|CustReplScheme;"
            f"snapshot.scheme=|FILE|{SCHEME_ROOT}/ordbook.ini|CustReplScheme"
        ),
        "scheme": "ordLog_trades.ini + ordbook.ini",
        "source_docs": ("docs/plaza2/P2ORDBOOK_ORDLOG_RECOVERY_9_9.md", "docs/plaza2/C2_CAPTURE_HARNESS_9_9.md"),
    },
)


def validate_sources() -> None:
    required = {
        "docs/plaza2/RAW_ORDLOG_C1.md": "p2repl://FORTS_ORDLOG_REPL",
        "docs/plaza2/PUBLIC_L3_MUTATION_CONTRACT_9_9.md": "p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL",
        "docs/plaza2/P2ORDBOOK_ORDLOG_RECOVERY_9_9.md": "ordLog_trades.ini",
        "tests/plaza2_cgate/plaza2_aggr20_md_validation_test.cpp": "p2repl://FORTS_ORDBOOK_REPL",
    }
    for relative, needle in required.items():
        if needle not in (ROOT / relative).read_text():
            raise SystemExit(f"qualified probe source changed: {relative}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise SystemExit("output file must be new")
    validate_sources()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "format": 1,
        "execution": "NOT_EXECUTED",
        "purpose": "isolate server-side public ORDLOG/ORDBOOK entitlement after written confirmation",
        "bounded_duration_ms": 5000,
        "open_attempts_per_mode": 1,
        "retry_count": 0,
        "publisher_calls": 0,
        "transaction_commands": 0,
        "orders": 0,
        "modes": [dict(mode, source_docs=list(mode["source_docs"])) for mode in MODES],
    }
    args.output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
