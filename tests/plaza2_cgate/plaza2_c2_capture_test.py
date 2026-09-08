#!/usr/bin/env python3
"""Execute the native recorder only against the fake CGate library, then import immutable traces."""
import importlib.util
import json
import os
import resource
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('capture_import', ROOT / 'tools/plaza2_c2_capture_import.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
BINARY, FAKE, ORACLE = (Path(p).resolve() for p in sys.argv[1:4])
ALLOWED = {'cg_env_open', 'cg_env_close', 'cg_conn_new', 'cg_conn_open', 'cg_conn_close', 'cg_conn_destroy',
           'cg_conn_getstate', 'cg_conn_process', 'cg_lsn_new', 'cg_lsn_open', 'cg_lsn_close', 'cg_lsn_destroy',
           'cg_lsn_getstate', 'cg_lsn_getscheme', 'cg_err_getstr', 'cg_env_getcomp_ver'}
SECRET = 'Secret-Capture-Credential-123'


def run(root, name, flags=None, options=None, write_failure=False, duration_ms=80):
    folder = root / name
    folder.mkdir()
    audit = folder / 'audit.txt'
    config = folder / 'capture.cfg'
    scheme = ROOT / 'protocols/plaza2_cgate/schema/plaza2_forts_reviewed.ini'
    config.write_text(f'runtime={FAKE}\nenv=key={SECRET}\nconnection=p2tcp://127.0.0.1:1;local_pass={SECRET}\n'
                      'regular=p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;name=c2_regular\n'
                      'multileg=p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;name=c2_multileg;'
                      'snapshot.data=multileg_orders;online.data=multileg_orders_log;snapshot.bind=info.trades_rev\n'
                      f'ordbook_scheme={scheme}\nordlog_scheme={scheme}\n')
    env = {**os.environ, 'MOEX_FAKE_CAPTURE_SCRIPT': '1', 'MOEX_FAKE_CAPTURE_AUDIT': str(audit), **(flags or {})}
    out = folder / 'evidence'
    def limit():
        resource.setrlimit(resource.RLIMIT_FSIZE, (8192, 8192))
    command = [str(BINARY), '--config', str(config), '--output', str(out), '--duration-ms', str(duration_ms)] + (options or [])
    result = subprocess.run(command, env=env, text=True, capture_output=True,
                            preexec_fn=limit if write_failure else None)
    assert SECRET not in result.stdout and SECRET not in result.stderr
    calls = Counter(audit.read_text().splitlines()) if audit.exists() else Counter()
    if not flags or not flags.get('MOEX_FAKE_CAPTURE_NO_AUDIT'):
        assert not (set(calls) - ALLOWED), calls
        assert sum(count for name, count in calls.items() if name.startswith('cg_pub_')) == 0
        assert 'AddOrder' not in calls and 'CancelOrder' not in calls and 'MoveOrder' not in calls
    for p in out.glob('*'):
        assert SECRET.encode() not in p.read_bytes(), f'credential leaked in {p.name}'
    if write_failure:
        assert result.returncode == 2 and not (out / 'capture.bin').exists(), (result, calls)
        return None
    if options and '--buffer-bytes' in options:
        assert result.returncode == 2
        metrics = json.loads((out / 'metrics.json').read_text())
        assert metrics['status'] == 'CAPTURE_INVALID' and metrics['capture_overflow'] > 0
        assert metrics['first_lost_ordinal'] > 0
        assert not (out / 'capture.bin').exists()
        return None
    assert result.returncode == 0, result.stderr
    trace = out / 'capture.bin'
    before = trace.read_bytes()
    analysis = module.derive(trace, out / 'derived', ORACLE)
    assert trace.read_bytes() == before
    assert analysis['production_c2'] == 'BLOCKED'
    if not flags or not flags.get('MOEX_FAKE_LSN_ERROR_STATE'):
        assert calls['cg_lsn_new'] >= 2 and calls['cg_lsn_getscheme'] >= 1
    if not flags or not flags.get('MOEX_FAKE_CAPTURE_NO_AUDIT'):
        assert calls['cg_env_close'] == 1 and calls['cg_conn_destroy'] == 1
    return out, analysis


symbols = subprocess.run(['nm', '-g', str(BINARY)], capture_output=True, text=True, check=True).stdout
assert 'Plaza2Publisher' not in symbols and 'cg_pub_' not in symbols and 'ConnectorHost' not in symbols

with tempfile.TemporaryDirectory(prefix='plaza2_c2_capture_') as tmp:
    root = Path(tmp)
    unsafe = root / 'untracked-env.cfg'
    unsafe.write_text(f'runtime={FAKE}\nenv=ini=/private/untracked.ini\nconnection=p2tcp://127.0.0.1:1\n'
                      'regular=p2ordbook://FORTS_ORDLOG_REPL;snapshot=FORTS_ORDBOOK_REPL;name=c2_regular\n'
                      'ordbook_scheme=/unused\nordlog_scheme=/unused\n')
    rejected_config = subprocess.run([str(BINARY), '--config', str(unsafe), '--output', str(root / 'unsafe')],
                                     capture_output=True)
    assert rejected_config.returncode == 2 and not (root / 'unsafe').exists()
    out, analysis = run(root, 'regular_multileg_reopen', options=['--reopen-ms', '5'])
    assert analysis['oracle']['equivalence'] == 'PASS_CONDITIONAL', analysis
    assert analysis['oracle']['common_frontier_comparisons'] >= 1
    assert len(analysis['outcomes']) == 3
    original = out / 'capture.bin'
    digest = module.sha(original)
    for variant in ('corrupt', 'truncated'):
        broken = root / f'{variant}.bin'
        data = bytearray(original.read_bytes())
        if variant == 'corrupt':
            data[len(data) // 2] ^= 1
        else:
            del data[-10:]
        broken.write_bytes(data)
        broken.with_name(broken.name + '.sha256').write_text(digest if variant == 'corrupt' else module.sha(broken))
        try:
            module.derive(broken, root / f'bad_{variant}')
            raise AssertionError('corrupt trace accepted')
        except ValueError:
            pass
    out, permuted = run(root, 'permuted', {'MOEX_FAKE_COMPOSITE_REVERSE': '1'})
    assert permuted['oracle']['ready_observations'] >= 2, permuted
    desc = json.loads((out / 'derived/descriptors.json').read_text())
    assert desc[0]['tables'][0]['name'] == 'orders'
    out, unknown = run(root, 'unknown_controls', {'MOEX_FAKE_CAPTURE_UNKNOWN': '1', 'MOEX_FAKE_CAPTURE_CONTROLS': '1'})
    rows = [json.loads(line) for line in (out / 'derived/records.jsonl').read_text().splitlines()]
    assert any(row.get('table') == 'future_table' for row in rows)
    assert {0x777, 0x1110, 0x1111, 0x101, 0x1115} <= {row['native_type'] for row in rows}
    assert unknown['oracle']['unknown_records'] > 0
    out, rejected = run(root, 'rejected_multileg', {'MOEX_FAKE_CAPTURE_REJECT_MULTILEG': '1'})
    assert 'OPEN_REJECTED_WITH_EXACT_ERROR' in rejected['outcomes'].values()
    controls = (out / 'derived/controls.jsonl').read_text()
    assert 'sdk_error_text' in controls and '131073' in controls and SECRET not in controls
    out, unexpected = run(root, 'unexpected_descriptor', {'MOEX_FAKE_ORDLOG_BAD_SCHEME': '1'})
    assert 'OPENED_BUT_UNEXPECTED_DESCRIPTOR' in unexpected['outcomes'].values()
    out, quiet = run(root, 'quiet', {'MOEX_FAKE_CAPTURE_QUIET': '1'})
    assert all(x['passive_activity'] == 'NOT_OBSERVED' for x in quiet['observations'].values())
    out, state_spin = run(root, 'state_error_spin',
                          {'MOEX_FAKE_LSN_ERROR_STATE': '1', 'MOEX_FAKE_CAPTURE_STATE_SPIN': '1',
                           'MOEX_FAKE_CAPTURE_NO_AUDIT': '1'},
                          duration_ms=600)
    state_controls = [json.loads(line) for line in (out / 'derived/controls.jsonl').read_text().splitlines()]
    state_errors = [row for row in state_controls if row.get('outcome') == 'STATE_ERROR']
    summaries = [row for row in state_controls if row.get('event') == 'listener_state_summary']
    assert len(state_errors) <= 4, len(state_errors)
    assert all(sum(row.get('listener') == listener for row in state_errors) <= 2
               for listener in {row.get('listener') for row in state_errors})
    assert any(row.get('unchanged_polls', 0) >= 100_000 for row in summaries), summaries
    run(root, 'overflow', options=['--buffer-bytes', '8192'])
    run(root, 'write_failure', write_failure=True)
    print('CAPTURE PASS: 9 native runs; trace roundtrip; permuted/unknown descriptors; controls; reopen oracle; '
          'corruption/truncation rejected; overflow/write failure invalid; all API calls allowlisted; publisher calls=0')
