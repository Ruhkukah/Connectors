#!/usr/bin/env python3
"""Structural guard: the connectivity qualifier must never expose or call a send API."""

from __future__ import annotations

import sys
from pathlib import Path


FORBIDDEN = (
    "post_by_message_name",
    "AddOrder",
    "DelOrder",
    "DelUserOrders",
    "cg_pub_post",
)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: assert_plaza2_qualifier_no_send.py <project-root>")
    root = Path(sys.argv[1])
    paths = (
        root / "protocols/plaza2_cgate/include/moex/plaza2/cgate/plaza2_trade_connectivity_qualifier.hpp",
        root / "protocols/plaza2_cgate/src/plaza2_trade_connectivity_qualifier.cpp",
        root / "apps/plaza2_test_connectivity_qualifier.cpp",
    )
    for path in paths:
        text = path.read_text(encoding="utf-8")
        for forbidden in FORBIDDEN:
            if forbidden in text:
                raise SystemExit(f"qualifier send boundary violated: {forbidden} in {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
