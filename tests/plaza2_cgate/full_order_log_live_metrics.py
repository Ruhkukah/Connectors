#!/usr/bin/env python3
"""Read-only /proc sampling. It never starts a process or opens a network connection."""
import argparse
import json
import os
from pathlib import Path
import time


def process_sample(pid, proc_root=Path('/proc')):
    root = proc_root / str(pid)
    tail = (root / 'stat').read_text().rsplit(') ', 1)[1].split()
    status = {}
    for line in (root / 'status').read_text().splitlines():
        key, _, value = line.partition(':')
        if key in ('VmRSS', 'Threads'):
            status[key] = int(value.strip().split()[0])
    return {'pid': pid, 'start_ticks': int(tail[19]),
            'cpu_ticks': int(tail[11]) + int(tail[12]),
            'rss_kib': status.get('VmRSS', 0), 'threads': status.get('Threads', 0)}


def cpu_percent(previous, current, seconds, ticks_per_second):
    if previous['start_ticks'] != current['start_ticks']:
        raise RuntimeError('PID was reused; original process identity retired')
    ticks = current['cpu_ticks'] - previous['cpu_ticks']
    if ticks < 0 or seconds <= 0:
        raise RuntimeError('invalid process sampling interval')
    return 100.0 * ticks / ticks_per_second / seconds


def trading_metric(path, expected_pid, now_ns):
    # Explicit owner-provided schema; no inferred p99 or arbitrary file content is emitted.
    if path is None:
        return {'available': False, 'reason': 'owner trading p99 source is missing'}
    data = json.loads(path.read_text())
    required = ('pid', 'metric_kind', 'p99_ns', 'sample_count', 'observed_unix_ns')
    if any(key not in data for key in required):
        return {'available': False, 'reason': 'owner trading metric schema is incomplete'}
    if data['pid'] != expected_pid or not isinstance(data['metric_kind'], str):
        return {'available': False, 'reason': 'trading metric process identity does not match'}
    if any(type(data[key]) is not int or data[key] < 0 for key in ('p99_ns', 'sample_count', 'observed_unix_ns')):
        return {'available': False, 'reason': 'invalid owner trading metric values'}
    age = now_ns - data['observed_unix_ns']
    if age < 0 or age > 30_000_000_000 or data['sample_count'] == 0:
        return {'available': False, 'reason': 'trading p99 is stale, future-dated or has no samples'}
    # Only the known scalar metric fields survive into evidence.
    return {'available': True, **{key: data[key] for key in required}}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--full-log-pid', type=int, required=True)
    p.add_argument('--trading-pid', type=int, required=True)
    p.add_argument('--trading-metrics', type=Path, required=True)
    p.add_argument('--seconds', type=int, default=1800)
    p.add_argument('--interval', type=float, default=1.0)
    args = p.parse_args()
    if args.full_log_pid == args.trading_pid or min(args.full_log_pid, args.trading_pid) <= 0:
        p.error('distinct positive FullLog and trading process PIDs are required')
    if not 1 <= args.seconds <= 3600 or not 0.1 <= args.interval <= 30:
        p.error('sampling duration/interval are outside their bounds')
    previous = {pid: process_sample(pid) for pid in (args.full_log_pid, args.trading_pid)}
    ticks = os.sysconf('SC_CLK_TCK')
    started = at = time.monotonic()
    missing = 0
    while at - started < args.seconds:
        time.sleep(min(args.interval, args.seconds - (at-started)))
        now = time.monotonic()
        processes = []
        for pid in previous:
            current = process_sample(pid)
            processes.append({**current, 'cpu_percent_one_core': cpu_percent(previous[pid], current, now-at, ticks)})
            previous[pid] = current
        metric = trading_metric(args.trading_metrics, args.trading_pid, time.time_ns())
        missing += not metric['available']
        print(json.dumps({'kind': 'process_metrics', 'observed_unix_ns': time.time_ns(),
                          'elapsed_seconds': now-started, 'processes': processes, 'trading_p99': metric}), flush=True)
        at = now
    print(json.dumps({'kind': 'process_metrics_result', 'elapsed_seconds': at-started,
                      'missing_trading_metric_samples': missing, 'all_samples_available': missing == 0}), flush=True)
    return 0 if missing == 0 else 3


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
        # Paths/content of private files are not echoed.
        print(json.dumps({'kind': 'process_metrics_failed', 'error_type': type(error).__name__}), flush=True)
        raise SystemExit(2)
