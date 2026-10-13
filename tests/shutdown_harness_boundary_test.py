"""Offline executable/isolation checks; never execute the native scenario path."""
import json
from pathlib import Path
import subprocess
import sys

harness, production = map(Path, sys.argv[1:])
for argv in ([], ['--execute-isolated-test'], ['--unknown', 'private-sentinel']):
    result = subprocess.run([str(harness), *argv], capture_output=True, text=True, check=False)
    assert result.returncode == 2, result
    receipt = json.loads(result.stdout)
    assert receipt == {'schema': 1, 'failure': 'invalid_input_or_host_construction', 'native_result': False}
    assert 'private-sentinel' not in result.stdout + result.stderr
result = subprocess.run([str(harness), '--help'], capture_output=True, text=True, check=True)
assert 'not a CLI signal or certification result' in result.stdout
for binary, injected in ((harness, True), (production, False)):
    symbols = subprocess.run(['nm', '-C', str(binary)], capture_output=True, text=True, check=True).stdout
    for hook in ('qualification_close_owned_connection', 'qualification_allow_reopen'):
        assert (hook in symbols) == injected, (binary.name, hook)
    boundary = b'isolated shutdown harness prohibits every publisher post'
    assert (boundary in binary.read_bytes()) == injected, binary.name
print('offline CLI refusal and production/harness isolation PASS')
