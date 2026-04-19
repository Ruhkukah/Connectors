#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path


REVIEWED = {
    "establish": {
        "hex": "20008813454d07000080cbdd69e7cc17e80300004c4f47494e000000000000000000000000000000",
        "certlog": (
            "Establish (blockLength=32, templateId=5000, schemaId=19781, "
            'version=7, Timestamp=1715000000000000000, KeepaliveInterval=1000, Credentials="LOGIN")'
        ),
    },
    "sequence_heartbeat": {
        "hex": "08008e13454d07001a00000000000000",
        "certlog": "Sequence (blockLength=8, templateId=5006, schemaId=19781, version=7, NextSeqNo=26)",
    },
    "new_order_single_day": {
        "hex": "2f007017454d07006600000000000000ffffffffffffffffa086010000000000564f050080797800050000005200010041414141000000",
        "certlog": (
            "NewOrderSingle (blockLength=47, templateId=6000, schemaId=19781, "
            "version=7, ClOrdID=102, ExpireDate=null, Price=100000e-5, "
            "SecurityID=347990, ClOrdLinkID=7895424, OrderQty=5, "
            'ComplianceID=Algorithm, TimeInForce=Day, Side=Buy, ClientFlags=0, Account="AAAA")'
        ),
    },
    "cancel_request": {
        "hex": "1c007617454d0700ca000000000000002958890000000000564f05000041414141000000",
        "certlog": (
            "OrderCancelRequest (blockLength=28, templateId=6006, schemaId=19781, "
            'version=7, ClOrdID=202, OrderID=9001001, SecurityID=347990, ClientFlags=0, Account="AAAA")'
        ),
    },
    "replace_request": {
        "hex": "2e007717454d0700cb0000000000000029588900000000007c8c0100000000000800000082797800564f050052010041414141000000",
        "certlog": (
            "OrderReplaceRequest (blockLength=46, templateId=6007, schemaId=19781, "
            "version=7, ClOrdID=203, OrderID=9001001, Price=101500e-5, "
            "OrderQty=8, ClOrdLinkID=7895426, SecurityID=347990, "
            'ComplianceID=Algorithm, Mode=ChangeOrderQty, ClientFlags=0, Account="AAAA")'
        ),
    },
    "execution_single_report": {
        "hex": (
            "4d006b1b454d07006600000000000000c489cbdd69e7cc172958890000000000"
            "55f8060000000000000000000000000000000000000000009a87010000000000"
            "0100000005000000b004000080797800564f050001"
        ),
        "certlog": (
            "ExecutionSingleReport (blockLength=77, templateId=7019, schemaId=19781, "
            "version=7, ClOrdID=102, Timestamp=1715000000000002500, "
            "OrderID=9001001, TrdMatchID=456789, Flags=0, Flags2=0, "
            "LastPx=100250e-5, LastQty=1, OrderQty=5, TradingSessionID=1200, "
            "ClOrdLinkID=7895424, SecurityID=347990, Side=Buy)"
        ),
    },
}


def main() -> int:
    project_root = Path(sys.argv[1]).resolve()
    expected_root = project_root / "tests" / "fixtures" / "twime_sbe" / "expected"
    for name, expected in REVIEWED.items():
        actual_hex = (expected_root / f"{name}.hex").read_text(encoding="utf-8").strip()
        actual_certlog = (expected_root / f"{name}.certlog").read_text(encoding="utf-8").strip()
        if actual_hex != expected["hex"]:
            raise SystemExit(f"reviewed hex drift for {name}")
        if actual_certlog != expected["certlog"]:
            raise SystemExit(f"reviewed certlog drift for {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
