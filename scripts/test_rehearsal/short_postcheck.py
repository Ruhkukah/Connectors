"""One publisher-free post-session inventory check; never sends trading commands."""
from pathlib import Path
import argparse
import datetime as dt
import fcntl
import json
import os
import shlex
import sys
import time


class PostcheckRefused(Exception):
    pass


def require(value, code):
    if not value:
        raise PostcheckRefused(code)


def integer(value, expected):
    return type(value) is int and value == expected


def bounds(cfg):
    zone = dt.timezone(dt.timedelta(hours=3))
    day = dt.date.fromisoformat(cfg['authorized_date'])
    at = dt.datetime.combine(day, dt.time(15, 10), zone).timestamp()
    end = dt.datetime.combine(day, dt.time(23, 40), zone).timestamp()
    require(cfg.get('post_session_check_utc') == at
            and cfg.get('post_session_check_deadline_utc') == end,
            'postcheck_exact_same_day_scope_missing')
    return at, end


def exact_alive(ops, identity):
    try:
        return ops.process_identity(identity['pid']) == identity
    except (FileNotFoundError, ProcessLookupError):
        return False


def no_owned_products(root, ops):
    # An attachment failure can leave active metadata pointing at the old PID.
    # Startup and emergency receipts independently identify the new child.
    root = Path(root)
    paths = [root / 'active-product-private.json',
             root / 'emergency-product-owned-private.json', root / 'day/product-owned.json']
    for path in paths:
        if path.exists():
            data = json.loads(path.read_text())
            identity = data.get('identity', data)
            require(not exact_alive(ops, identity), 'postcheck_owned_product_still_alive')


def prepare_private_card(root, launch):
    """Seal the exact launch context on TEST; this function executes nothing."""
    root = Path(root)
    require(root.stat().st_uid == os.getuid() and not root.stat().st_mode & 0o077,
            'contingency_root_not_private_owned')
    argv = [os.fsdecode(value) for value in launch.argv]
    env = {os.fsdecode(key): os.fsdecode(value) for key, value in launch.env.items()}
    require(argv and all('=' not in key and key for key in env), 'contingency_launch_context_invalid')
    command = 'cd -- ' + shlex.quote(str(launch.cwd)) + '\n' + shlex.join(
        ['env', '-i', *[key+'='+value for key, value in sorted(env.items())], *argv])
    card = (root / 'SHORT_CONTINGENCY.md').read_text()
    card += '\n## PRIVATE: exact recorded router relaunch command\n\n'
    card += 'Only after identity/absence and fault-lock checks; never beside a live or unknown router.\n\n'
    card += '```sh\n' + command + '\n```\n'
    card += '\n## PRIVATE: obtain commands for the actual current product\n\n'
    card += 'This prints commands only. Review current PID/socket/ownership before executing each once.\n\n'
    card += "```sh\npython3 - <<'PY'\nimport json, shlex\nfrom pathlib import Path\n"
    card += 'm=json.loads(Path('+repr(str(root / 'active-product-private.json'))+').read_text())\n'
    card += "a=m['args']; s=a[a.index('--command-socket')+1]\n"
    card += "base=[m['binary'],'plaza2','cmd','--command-socket',s]\n"
    card += "print(shlex.join(base+['kill on']))\n"
    card += "for t in m['targets']: print(shlex.join(base+['cancel-all '+str(t['isin_id'])]))\nPY\n```\n"
    path = root / 'CONTINGENCY_PRIVATE.md'
    with path.open('x', encoding='utf-8') as stream:
        stream.write(card)
    path.chmod(0o600)
    return path


def check_once(root, cfg, runtime, ops, meta, now=time.time):
    """Observe once after the old product stopped, including BLOCKED sessions."""
    at, end = bounds(cfg)
    require(at <= now() < end, 'postcheck_outside_authorized_same_day_window')
    require(not exact_alive(ops, meta['identity']), 'postcheck_owned_product_still_alive')
    before = runtime.inventory(meta)
    require(integer(before.get('client_count'), 0) and integer(before.get('upstreams'), 4),
            'postcheck_idle_router_not_proved')
    checkpoint = runtime.sha(meta['state'])
    label = 'post-session-independent-flat'
    probe_meta = dict(meta, root=str(root), authorized_date=cfg['authorized_date'])
    try:
        result = runtime.readonly_flat(probe_meta, label)
    except Exception:
        # A complete nonflat result is actionable. Incomplete snapshots, an
        # unknown process exit or transport failure never imply flatness.
        path = Path(root) / label / 'snapshot-observation-public.json'
        observation = json.loads(path.read_text()) if path.exists() else {}
        complete = (observation.get('fresh_private_snapshot_ready') is True
            and observation.get('stop_ok') is True
            and type(observation.get('exit')) is int and observation['exit'] == 5
            and integer(observation.get('publisher_posts'), 0)
            and integer(observation.get('message_allocations'), 0)
            and observation.get('checkpoint_unchanged_by_probe') is True
            and integer((observation.get('after') or {}).get('client_count'), 0)
            and type(observation.get('working_orders')) is int
            and type(observation.get('nonzero_positions')) is int
            and observation['working_orders'] >= 0 and observation['nonzero_positions'] >= 0
            and (observation['working_orders'] > 0 or observation['nonzero_positions'] > 0))
        if complete:
            return dict(state='NONFLAT_ACTION_REQUIRED', flat=False,
                working_orders=observation['working_orders'],
                nonzero_positions=observation['nonzero_positions'],
                cancel_confirmed='UNKNOWN', independent_observation=observation,
                trading_commands=0, automatic_recovery=False)
        raise
    after = runtime.inventory(meta)
    require(integer(result.get('complete_replication_streams'), 8)
            and integer(result.get('working_orders'), 0) and integer(result.get('nonzero_positions'), 0)
            and type(result.get('exit')) is int and result['exit'] == 0
            and integer(result.get('publisher_posts'), 0) and integer(result.get('message_allocations'), 0)
            and result.get('checkpoint_unchanged_by_probe') is True
            and integer((result.get('after') or {}).get('client_count'), 0)
            and integer(after.get('client_count'), 0) and integer(after.get('upstreams'), 4)
            and runtime.sha(meta['state']) == checkpoint,
            'postcheck_independent_flatness_unproved')
    return dict(state='INDEPENDENT_FLAT_CONFIRMED', flat=True,
        working_orders=0, nonzero_positions=0, after=after,
        independent_observation=result, trading_commands=0, automatic_recovery=False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--execute', action='store_true', required=True)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    sys.path.insert(0, str(root))
    import runtime
    import ops
    import base_supervisor
    def publish(**fields):
        runtime.atomic(root / 'postcheck-public.json', dict(fields,
            updated_utc_ns=time.time_ns(), same_day_only=True,
            trading_commands=0, automatic_recovery=False, original_verdict_unchanged=True))
    try:
        cfg = runtime.load_cfg(root)
        runtime.verify_ci(root, cfg)  # The manifest-pinned receipt, never GitHub.
        load = lambda name, logical: base_supervisor.module_from(root, name, logical)
        load('supervisor.py', 'short_supervisor').load_fault_helpers(load, runtime, cfg)
        at, end = bounds(cfg)
        runtime.atomic(root / 'postcheck-owned.json', ops.process_identity(os.getpid()))
        publish(state='WAIT_POST_SESSION', scheduled_utc=at, deadline_utc=end)
        while time.time() < at:
            time.sleep(min(5, max(0, at-time.time())))
        require(time.time() < end, 'postcheck_same_day_deadline_missed')
        require(runtime.sha('/proc/sys/kernel/random/boot_id') == runtime.BOOT,
                'postcheck_boot_changed')
        no_owned_products(root, ops)
        meta_path = root / 'active-product-private.json'
        if not meta_path.exists():
            meta_path = root / 'postcheck-seed-private.json'
        require(meta_path.exists(), 'postcheck_no_product_metadata_no_probe')
        meta = json.loads(meta_path.read_text())
        lock = Path(cfg['fault_lock'])
        require(not lock.is_symlink(), 'postcheck_fault_lock_symlink')
        with lock.open('a') as owner:
            fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
            publish(**check_once(root, cfg, runtime, ops, meta))
    except Exception as error:
        code = str(error) if isinstance(error, PostcheckRefused) else type(error).__name__
        publish(state='UNKNOWN_ACTION_REQUIRED', flat='UNKNOWN', cancel_confirmed='UNKNOWN',
                failure_code=code)


if __name__ == '__main__':
    main()
