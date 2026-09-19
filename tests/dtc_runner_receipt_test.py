"""Strict-UTF-8 validation of the actual DTC runner startup receipt."""

import json
import subprocess
import sys


result = subprocess.run([sys.argv[1]], check=True, capture_output=True)
receipt = json.loads(result.stdout.decode("utf-8", errors="strict"))
raw = bytes.fromhex("d090202271756f74656422205c00011fff")
expected_text = 'А "quoted" \\\x00\x01\x1f\ufffd'
fields = (
    "symbol",
    "underlying_board",
    "currency",
    "min_step",
    "description",
    "contract_size",
    "currency_value_per_increment",
    "future_vcb_base_contract_code",
)

assert receipt["metadata_507_ready"] is False
assert receipt["metadata_507_reason"]
assert receipt["application_declared_capabilities"]["security_definitions"] is True
assert receipt["wire_logon_capabilities"]["response_fully_written_to_socket"] is False
assert set(receipt["invalid_utf8_raw_hex"]) == set(fields)
for field in fields:
    assert receipt[field] == expected_text, field
    assert receipt["invalid_utf8_raw_hex"][field] == raw.hex(), field
