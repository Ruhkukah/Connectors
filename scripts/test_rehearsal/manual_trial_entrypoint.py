#!/usr/bin/env python3
"""Explicit role entry point for the sealed 22 October manual owner composition.

Importing this file starts nothing. No trading scenario, fault/recovery main,
replacement, timer or command sequence is invoked here.
"""
import argparse
import fcntl
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import sys
import time
import traceback

SOURCE = 'b1911621318603bf3b6d7278073a0266dc1ee0e3'
BINARY = 'c63d3021207fe80e4a62d265757294d8db3bff2cd4ccac9619b1edbcbe650c38'


class Refused(RuntimeError):
    pass


def require(value, code):
    if not value:
        raise Refused(code)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def private(path, directory=False):
    path = Path(path)
    require(not path.is_symlink() and path.stat().st_uid == os.getuid()
            and not path.stat().st_mode & 0o077
            and (path.is_dir() if directory else path.is_file()), 'private_owned_path_required')
    return path


def sealed_root(root, manifest_sha):
    original = Path(root)
    require(original.is_absolute() and not original.is_symlink(), 'absolute_real_root_required')
    root = private(original.resolve(strict=True), directory=True)
    require(root.is_relative_to((Path.home()/'moex/qualification').resolve()), 'protected_qualification_root_required')
    manifest_path = private(root/'manifest.json')
    require(re.fullmatch('[0-9a-f]{64}', manifest_sha) is not None
            and sha(manifest_path) == manifest_sha, 'sealed_manifest_changed')
    manifest = json.loads(manifest_path.read_text())
    require(manifest.get('source_git_sha') == SOURCE and manifest.get('binary_sha256') == BINARY,
            'sealed_native_provenance_changed')
    pins = manifest.get('files')
    require(isinstance(pins, dict) and pins, 'sealed_files_missing')
    for name, pin in pins.items():
        require(isinstance(name, str) and not Path(name).is_absolute()
                and '..' not in Path(name).parts, 'sealed_file_path_outside_root')
        path = private(root/name)
        require(path.resolve().is_relative_to(root) and sha(path) == pin, 'sealed_file_changed')
    required = ('configuration-private.json', 'manual_trial_entrypoint.py', 'manual_trial_day.py', 'runtime.py', 'frozen_runtime.py',
                'supervisor.py', 'base_supervisor.py', 'ops.py', 'bootstrap.py', 'guardian.py',
                'coverage_proof.py', 'day_driver.py', 'router_recovery.py', 'upstream_recovery.py',
                'short_fault_helpers.py')
    require(all(name in pins for name in required), 'required_composition_file_unsealed')
    return root


def setup(root):
    """Canonical helpers are imported/installed, never run as fault programs."""
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(root))
    def canonical(name):
        module = importlib.import_module(name)
        require(Path(module.__file__).resolve().parent == root, 'canonical_helper_import_mismatch')
        return module
    manual = canonical('manual_trial_day')
    rt = canonical('runtime')
    manual.install_runtime_scope(rt)  # Must precede runtime.load_cfg, in BOTH roles.
    cfg = rt.load_cfg(root)
    manual.validate(cfg, held=False)
    ops, short = canonical('ops'), canonical('supervisor')
    # Installs the reviewed canonical Refused class/config whitelist adapters.
    # This function only binds helpers; it performs no fault, signal or start.
    short.load_fault_helpers(lambda name, logical: short.base.module_from(root, name, logical), rt, cfg)
    for name in ('router_recovery', 'upstream_recovery', 'short_fault_helpers',
                 'base_supervisor', 'frozen_runtime'):
        canonical(name)
    components = dict(manual=manual, runtime=rt, cfg=cfg, ops=ops, short=short,
                      guardian=canonical('guardian'))
    return components


def execute(root, manifest_sha, role):
    require(sys.platform == 'linux', 'linux_TEST_host_required')
    root = sealed_root(root, manifest_sha)
    parts = setup(root)
    rt, cfg, ops = parts['runtime'], parts['cfg'], parts['ops']
    # Same lock names as the immutable owner/guardian protocol. Never wait to
    # become a second owner after a live predecessor eventually releases it.
    lock_name = 'supervisor.lock' if role == 'owner' else 'guardian.lock'
    fd = os.open(root/lock_name, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'a') as lock:
        private(root/lock_name)
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if role == 'guardian':
            parts['guardian'].Guardian(root, cfg, rt, ops).run()
            done = json.loads(private(root/'guardian-done.json').read_text())
            require(done.get('state') == 'SUPERVISOR_TERMINAL_VERIFIED'
                    and done.get('no_guardian_failure') is True, 'guardian_did_not_verify_normal_terminal')
            return 0
        def component(name):
            module = importlib.import_module(name)
            require(Path(module.__file__).resolve().parent == root, 'canonical_component_import_mismatch')
            return module
        owner = parts['manual'].owner_class(parts['short'])(root, cfg, rt, ops,
                    component('bootstrap'), component('day_driver'), parts['guardian'], component('coverage_proof'))
        try:
            owner.run()
            return 0
        except BaseException as error:
            # Existing bounded cleanup and actual initiating verdict are retained.
            # The manual owner rejects restart; no replacement is started here.
            owner.fail(error)
            return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True)
    parser.add_argument('--manifest-sha', required=True)
    parser.add_argument('--role', required=True, choices=('owner', 'guardian'))
    parser.add_argument('--execute', action='store_true', required=True)
    args = parser.parse_args(argv)
    os.umask(0o077)
    try:
        require(args.execute is True, 'explicit_role_execution_required')
        return execute(args.root, args.manifest_sha, args.role)
    except BaseException as error:
        # No argv, settings, paths, accounts or raw exception text leave TEST.
        # A root validated by the seal owns private traceback evidence.
        try:
            root = sealed_root(args.root, args.manifest_sha)
            path = root/('entrypoint-'+args.role+'-error-private.txt')
            flags = os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW
            with os.fdopen(os.open(path, flags, 0o600), 'a') as output:
                private(path)
                output.write(traceback.format_exc())
                output.flush(); os.fsync(output.fileno())
        except BaseException:
            pass
        code = str(error) if isinstance(error, Refused) else type(error).__name__
        print(json.dumps(dict(status='ENTRYPOINT_REFUSED_OR_FAILED', role=args.role,
                              code=code, utc_ns=time.time_ns(), no_replacement_started=True)), flush=True)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
