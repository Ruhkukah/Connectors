"""Safe adapters on canonical, immutable router/upstream helper modules.

All process actions still require their frozen TEST authorization, binary,
ownership, launch-context and pristine endpoint checks. Import/install are inert.
There is no SIGKILL and a live, unknown or unverified router is never replaced.
"""
from pathlib import Path
import configparser
import fcntl
import os
import re
import signal
import socket
import sys
import time

T1_LINKS_SHA = 'ff64e85ac33958025c928f7cafd5da1804e35406bf3f016058f13e5826ed597a'


def canonical(router, upstream):
    router.require(sys.modules.get('router_recovery') is router and upstream.frozen is router and
                   upstream.Refused is router.Refused, 'canonical_router_helper_identity_unproved')


class Helpers:
    def __init__(self, router, upstream, preflight_deadline=None):
        self.r, self.u = router, upstream
        self.preflight_deadline = preflight_deadline
        self.original_whitelist = upstream.pristine_whitelist
        self.original_local = upstream.local_connection
        self.cache = {}

    def files(self, probe, launch):
        """The small exact files the frozen package/config proof relied on."""
        files = set()
        for value in launch.argv:
            token = os.fsdecode(value).split('=', 1)[-1]
            if token.lower().endswith(('.ini', '.cfg', '.conf')):
                path = Path(token)
                path = path if path.is_absolute() else launch.cwd / path
                if path.is_file():
                    files.add(path.resolve(strict=True))
        for directory in (launch.cwd, probe.executable.parent):
            for path in directory.iterdir():
                if path.suffix.lower() in ('.ini', '.cfg', '.conf') and path.is_file():
                    files.add(path.resolve(strict=True))
        self.r.require(len(files) == 1, 'actual_router_config_not_unique')
        config = next(iter(files))
        ini = configparser.ConfigParser(strict=False, interpolation=None)
        ini.read(config)
        name = ini['ROUTEINFO']['connections_ini'].strip().strip('"')
        name = name.split('|FILE|', 1)[-1].split('|', 1)[0].split(';', 1)[0].strip()
        name = re.sub(r'\$\{([A-Za-z0-9_]+)\}',
                      lambda match: launch.env.get(match[1].encode(), b'').decode(), name)
        path = Path(name).expanduser()
        candidates = [path] if path.is_absolute() else [launch.cwd / path, config.parent / path]
        candidates = {path.resolve(strict=True) for path in candidates if path.is_file()}
        self.r.require(len(candidates) == 1, 'actual_t1_config_not_unique')
        files.update(candidates)
        return tuple(sorted((str(path), self.r.digest(path)) for path in files))

    def pristine_whitelist(self, probe, launch, expected_sha, vendor=None):
        context = (str(probe.executable), expected_sha, tuple(launch.argv),
                   tuple(sorted(launch.env.items())), str(launch.cwd), str(vendor))
        files = self.files(probe, launch)
        if context in self.cache:
            old_files, whitelist = self.cache[context]
            self.r.require(files == old_files, 'cached_t1_configuration_changed')
            return whitelist
        until = min(self.preflight_deadline or float('inf'), time.time() + 30)
        while True:
            try:
                whitelist = self.original_whitelist(probe, launch, expected_sha, vendor=vendor)
                break
            except socket.gaierror:
                self.r.require(time.time() < until, 'pristine_t1_dns_retry_deadline')
                time.sleep(.2)
        self.r.require(self.files(probe, launch) == files, 't1_configuration_changed_during_resolution')
        self.cache[context] = files, whitelist
        return whitelist

    def restored(self, rows, whitelist):
        # DNS can return several approved addresses per configured endpoint.
        # Socket identity, ownership and whitelist membership remain strict.
        return (len(rows) == 4 and len({row.inode for row in rows}) == 4 and
                all(row.peer in whitelist.peers for row in rows))

    def launch_context(self, probe, launch, pid):
        start = probe.identity(pid, probe.executable)
        process = probe.proc / str(pid)
        argv = [part for part in (process / 'cmdline').read_bytes().split(b'\0') if part]
        env = dict(part.split(b'=', 1) for part in (process / 'environ').read_bytes().split(b'\0') if b'=' in part)
        self.r.require(argv == launch.argv and env == launch.env and
                       (process / 'cwd').resolve(strict=True) == launch.cwd,
                       'router_restore_launch_context_changed')
        self.r.require(probe.identity(pid, probe.executable) == start, 'router_restore_identity_changed')
        return start

    def wait_owned_exit(self, probe, launch, owner_pid, timeout=30):
        """Wait, then repeat TERM only to the exact original owned process."""
        for turn in range(2):
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                routers = probe.owned_routers()
                self.r.require(len(routers) <= 1 and all(exe == probe.executable for _, exe in routers),
                               'router_restore_unknown_process_present')
                if not routers:
                    return
                self.r.require(routers[0][0] == launch.pid, 'router_restore_original_identity_changed')
                try:
                    self.r.require(self.launch_context(probe, launch, launch.pid) == launch.start,
                                   'router_restore_original_identity_changed')
                except (FileNotFoundError, ProcessLookupError):
                    self.r.require(not probe.owned_routers(), 'router_restore_identity_unproved_after_exit')
                    return
                time.sleep(.1)
            if turn == 0:
                # Never signal a PID after a failed identity check and never
                # launch beside a slow router. A second TERM is risk reducing.
                try:
                    self.r.require(self.launch_context(probe, launch, launch.pid) == launch.start,
                                   'router_restore_original_identity_changed')
                    probe.clients(launch.pid, owner_pid, exact=False)
                    os.kill(launch.pid, signal.SIGTERM)
                except (FileNotFoundError, ProcessLookupError):
                    self.r.require(not probe.owned_routers(), 'router_restore_identity_unproved_after_exit')
                    return
        self.r.require(not probe.owned_routers(), 'owned_router_did_not_exit_after_bounded_sigterm')

    def rollback(self, probe, launch, owner_pid, output):
        time.sleep(self.r.ROLLBACK_DELAY)
        with (output / 'launch.lock').open('a') as arbiter:
            fcntl.flock(arbiter, fcntl.LOCK_EX)
            intent_path = output / 'router-stop-intent.json'
            if not intent_path.exists() and probe.owned_routers():
                # Pre-signal validation may have refused the fault after the
                # independent worker was armed. Never create a new fault then.
                try:
                    self.r.require(self.launch_context(probe, launch, launch.pid) == launch.start,
                                   'untouched_router_identity_changed')
                    probe.clients(launch.pid, owner_pid, exact=False)
                    white = self.u.pristine_whitelist(probe, launch, T1_LINKS_SHA)
                    self.r.require(self.restored(self.u.upstreams(probe, launch.pid, white), white),
                                   'untouched_router_health_unproved')
                    self.r.save(output / 'rollback.json', dict(status='original_router_untouched', restored=True,
                        router_count=1, router_hash_matched=True, utc_ns=time.time_ns()))
                    return 0
                except (FileNotFoundError, ProcessLookupError):
                    self.r.require(not probe.owned_routers(), 'untouched_router_identity_unproved_after_exit')
            if intent_path.exists():
                intent = self.r.json.loads(intent_path.read_text())
                self.r.require(intent == dict(pid=launch.pid, start=launch.start),
                               'router_stop_intent_identity_changed')
            self.wait_owned_exit(probe, launch, owner_pid)
            router = self.u.launch_router(probe, launch, output)
            until = time.monotonic() + 30
            while time.monotonic() < until:
                try:
                    self.r.require(router.poll() is None, 'restored_router_exited')
                    self.launch_context(probe, launch, router.pid)
                    probe.clients(router.pid, owner_pid, exact=False)
                    self.r.save(output / 'rollback.json', dict(status='router_restored', restored=True,
                        router_count=1, router_hash_matched=True, utc_ns=time.time_ns()))
                    return 0
                except (OSError, self.r.Refused):
                    time.sleep(.1)
        self.r.save(output / 'rollback.json', dict(status='router_restore_not_verified', restored=False))
        return 3

    def restart(self, probe, config, owner_pid, output, arm=None, send_signal=os.kill):
        self.r.validate_authorization(config, owner_pid)
        launch, before = probe.inspect(config, owner_pid)
        self.r.save(output / 'preflight.json', before)
        child = (arm or self.r.arm_rollback)(probe, launch, owner_pid, output)
        latest, _ = probe.inspect(config, owner_pid)
        self.r.require((latest.pid, latest.start) == (launch.pid, launch.start), 'router_changed_before_signal')
        self.r.validate_authorization(config, owner_pid)
        self.r.save(output / 'router-stop-intent.json', dict(pid=launch.pid, start=launch.start))
        send_signal(launch.pid, signal.SIGTERM)
        self.r.save(output / 'fault.json', dict(signal_sent=True, fault='LOCAL_ROUTER_RESTART',
                    upstream_fault=False, rollback_armed=True, utc_ns=time.time_ns()))
        until = time.monotonic() + 100
        while time.monotonic() < until:
            done, status = os.waitpid(child, os.WNOHANG)
            if done:
                self.r.require(status == 0, 'router_rollback_not_verified')
                receipt = self.r.json.loads((output / 'rollback.json').read_text())
                self.r.require(receipt.get('restored') is True, 'router_rollback_receipt_missing')
                return dict(status='router_restored', fault='LOCAL_ROUTER_RESTART', upstream_fault=False,
                            rollback_armed=True, native_recovery_proof='supervisor_status_before_after_required')
            time.sleep(.1)
        raise self.r.Refused('rollback_wait_deadline')

    def restore_if_needed(self, probe, launch, owner_pid, output, whitelist, initial_peers):
        # This arbiter has a separate open-file description in each process.
        # Hold it through every health check and replacement decision, including
        # the worker's original-router wait/launch. The shared fault lock alone
        # cannot serialize parent and watchdog (both inherited the same fd).
        with (output / 'launch.lock').open('a') as arbiter:
            fcntl.flock(arbiter, fcntl.LOCK_EX)
            until = time.monotonic() + 30
            while time.monotonic() < until:
                routers = probe.owned_routers()
                self.r.require(len(routers) <= 1 and all(exe == probe.executable for _, exe in routers),
                               'watchdog_other_router_present')
                if not routers:
                    router = self.u.launch_router(probe, launch, output)
                    routers = [(router.pid, probe.executable)]
                if routers[0][0] == launch.pid and (output / 'router-stop-intent.json').exists():
                    intent = self.r.json.loads((output / 'router-stop-intent.json').read_text())
                    self.r.require(intent == dict(pid=launch.pid, start=launch.start),
                                   'router_stop_intent_identity_changed')
                    # A router with an approved, durable TERM intent may still
                    # look healthy during signal delivery/shutdown. It cannot
                    # be accepted as a stable restoration and then die later.
                    self.wait_owned_exit(probe, launch, owner_pid)
                    until = time.monotonic() + 30
                    continue
                try:
                    pid = routers[0][0]
                    self.launch_context(probe, launch, pid)
                    probe.clients(pid, owner_pid, exact=False)
                    rows = self.u.upstreams(probe, pid, whitelist)
                    if self.restored(rows, whitelist):
                        self.r.save(output / 'rollback.json', dict(restored=True, router_count=1,
                            router_hash_matched=True, all_peers_pristine_t1=True, utc_ns=time.time_ns()))
                        return 0
                except (OSError, self.r.Refused):
                    pass
                time.sleep(.1)
        # No blind replacement of a live router, even when it is slow. The
        # guardian sees a strict failed restoration and stops further entry.
        self.r.save(output / 'rollback.json', dict(restored=False, status='router_health_not_verified',
                                                 utc_ns=time.time_ns()))
        return 3

    def local_connection(self, probe, router_pid, owner_pid, allow_reconnect=False):
        # Retry a complete scan; a second socket is transient only if BOTH ends
        # belong to the same exact product. A foreign client remains fatal.
        try:
            return self.original_local(probe, router_pid, owner_pid, allow_reconnect)
        except self.r.Refused as error:
            if not allow_reconnect or str(error) != 'other_router_clients':
                raise
            router_start = probe.identity(router_pid, probe.executable)
            product_exe = (probe.proc / str(owner_pid) / 'exe').resolve(strict=True)
            owner_start = probe.identity(owner_pid, product_exe)
            owned, product_owned, rows = probe.sockets(router_pid), probe.sockets(owner_pid), probe.tcp()
            connections = [row for row in rows if self.u.endpoint(row[0])[1] == 4101 and row[2] == '01']
            self.r.require(len(connections) == 2, 'other_router_clients')
            for local, peer, _, inode in connections:
                paired = [row for row in rows if row[:3] == (peer, local, '01')]
                self.r.require(inode in owned and len(paired) == 1 and paired[0][3] in product_owned,
                               'other_router_clients')
            self.r.require(probe.identity(router_pid, probe.executable) == router_start and
                           probe.identity(owner_pid, product_exe) == owner_start,
                           'loopback_reconnect_identity_changed')
            return None  # Pending, never healthy and never accepts another client.

    def wait_child_ready(self, probe, router, owner_pid, whitelist, initial_peers):
        until = time.monotonic() + self.u.STARTUP_TIMEOUT
        while time.monotonic() < until:
            self.r.require(router.poll() is None, 'child_router_exited')
            try:
                self.u.child_identity(probe, router.pid)
                probe.clients(router.pid, owner_pid)
                rows = self.u.upstreams(probe, router.pid, whitelist)
                if self.restored(rows, whitelist):
                    return rows
            except (OSError, self.r.Refused):
                pass
            time.sleep(.1)
        raise self.r.Refused('child_router_recovery_deadline')

    def interrupt(self, probe, config, owner_pid, output, keep_fd, acknowledge=None):
        u, r = self.u, self.r
        u.validate(config, owner_pid)
        u.ensure_pidfd_api()
        executable = Path(config['binary']).resolve(strict=True)
        r.require(r.digest(executable) == config['binary_sha256'], 'native_owner_binary_hash_changed')
        owner_start = probe.identity(owner_pid, executable)
        launch, before = u.inspect_retry(probe, config, owner_pid)
        whitelist = u.pristine_whitelist(probe, launch, config['test_upstream_file_sha256'])
        initial = u.upstreams(probe, launch.pid, whitelist)
        r.require(self.restored(initial, whitelist), 'four_whitelisted_upstreams_unproved')
        initial_peers = {row.peer for row in initial}
        r.save(output / 'preflight.json', {**before, 'test_upstream_verified': True,
               'upstream_count': len(initial), 'pristine_t1_sha256': whitelist.file_sha256})
        watchdog_started = time.monotonic()
        watchdog, control = u.arm_watchdog(probe, launch, owner_pid, output, whitelist, initial_peers, keep_fd)
        duplicates, completed = [], False
        try:
            latest, _ = u.inspect_retry(probe, config, owner_pid)
            r.require((latest.pid, latest.start) == (launch.pid, launch.start), 'router_changed_before_term')
            u.validate(config, owner_pid)
            # Watchdog cannot enter its restoration decision while the worker
            # is waiting for this exact router or starting its replacement.
            with (output / 'launch.lock').open('a') as arbiter:
                fcntl.flock(arbiter, fcntl.LOCK_EX)
                u.validate(config, owner_pid)
                r.require(self.launch_context(probe, launch, launch.pid) == launch.start,
                          'router_changed_before_term')
                r.save(output / 'router-stop-intent.json', dict(pid=launch.pid, start=launch.start))
                os.kill(launch.pid, signal.SIGTERM)
                r.save(output / 'parent-setup.json', dict(router_restart_sent=True, rollback_armed=True,
                                                        upstream_fault=False, utc_ns=time.time_ns()))
                self.wait_owned_exit(probe, launch, owner_pid)
                router = u.launch_router(probe, launch, output)
                r.save(output / 'child-private.json', dict(pid=router.pid, start=u.child_identity(probe, router.pid)))
                u.wait_child_ready(probe, router, owner_pid, whitelist, initial_peers)
            (acknowledge or u.acknowledge_native)()
            u.require_watchdog_live(watchdog, watchdog_started)
            u.validate(config, owner_pid)
            latest, _ = u.inspect_retry(probe, config, owner_pid)
            r.require(latest.pid == router.pid, 'router_changed_before_upstream_fault')
            start = u.child_identity(probe, router.pid)
            local_inode = u.local_connection(probe, router.pid, owner_pid)
            old = u.upstreams(probe, router.pid, whitelist)
            r.require(self.restored(old, whitelist), 'four_pre_cut_whitelisted_upstreams_unproved')
            u.require_watchdog_live(watchdog, watchdog_started)
            duplicates = u.duplicate_verified(probe, router.pid, start, old, whitelist)
            r.require(u.local_connection(probe, router.pid, owner_pid) == local_inode,
                      'local_connection_changed_before_fault')
            r.require(u.child_identity(probe, router.pid) == start, 'child_router_identity_changed')
            old_inodes = {row.inode for row in old}
            began = time.monotonic()
            interrupted = 0
            r.save(output / 'fault.json', dict(fault='TEST_UPSTREAM_SOCKET_INTERRUPTION', upstream_fault=False,
                fault_attempted=True, planned_socket_count=len(old), interrupted_socket_count=0,
                rollback_armed=True, utc_ns=time.time_ns()))
            u.require_watchdog_live(watchdog, watchdog_started)
            for duplicate in duplicates:
                duplicate.shutdown(socket.SHUT_RDWR)
                interrupted += 1
                r.save(output / 'fault.json', dict(fault='TEST_UPSTREAM_SOCKET_INTERRUPTION', upstream_fault=True,
                    fault_attempted=True, planned_socket_count=len(old), interrupted_socket_count=interrupted,
                    rollback_armed=True, utc_ns=time.time_ns()))
            for duplicate in duplicates:
                duplicate.close()
            duplicates = []
            severed = loopback_gap = loopback_reconnected = False
            until = began + u.UPSTREAM_TIMEOUT
            while time.monotonic() < until:
                r.require(router.poll() is None and u.child_identity(probe, router.pid) == start,
                          'router_died_during_upstream_fault')
                r.require(probe.identity(owner_pid, executable) == owner_start, 'native_owner_identity_changed')
                current_local = u.local_connection(probe, router.pid, owner_pid, allow_reconnect=True)
                loopback_gap |= current_local is None
                loopback_reconnected |= current_local is not None and current_local != local_inode
                established = {row[3] for row in probe.tcp() if row[2] == '01'}
                severed |= old_inodes.isdisjoint(established)
                try:
                    current = u.upstreams(probe, router.pid, whitelist)
                except r.Refused as error:
                    if str(error) != 'upstream_inventory_not_unique':
                        raise
                    current = []
                fresh = [row for row in current if row.inode not in old_inodes]
                if severed and current_local is not None and self.restored(fresh, whitelist):
                    latest, _ = u.inspect_retry(probe, config, owner_pid)
                    r.require(latest.pid == router.pid and latest.start == start, 'router_changed_after_upstream_fault')
                    r.require(probe.identity(owner_pid, executable) == owner_start, 'native_owner_identity_changed')
                    completed = True
                    return dict(stage='UPSTREAM_RESTORED', fault='TEST_UPSTREAM_SOCKET_INTERRUPTION',
                        upstream_fault=True, router_direct_child=True, router_process_preserved=True,
                        native_loopback_preserved=not loopback_gap and not loopback_reconnected,
                        native_loopback_gap_observed=loopback_gap,
                        native_loopback_reconnected=loopback_reconnected, owned_native_loopback_restored=True,
                        old_upstream_sockets_severed=True, fresh_test_upstreams_restored=True,
                        interrupted_socket_count=len(old), restored_socket_count=len(fresh),
                        socket_recovery_ms=round((time.monotonic() - began) * 1000), rollback_armed=True,
                        new_destinations_configured=0, privileges_changed=False,
                        native_recovery_proof='supervisor_status_after_required')
                time.sleep(.05)
            raise r.Refused('upstream_socket_recovery_deadline')
        finally:
            for duplicate in duplicates:
                duplicate.close()
            try:
                os.write(control, b'D' if completed else b'E')
            except OSError:
                pass
            os.close(control)
            until = time.monotonic() + 95
            while time.monotonic() < until:
                done, status = os.waitpid(watchdog, os.WNOHANG)
                if done:
                    r.require(status == 0, 'independent_router_rollback_not_verified')
                    break
                time.sleep(.1)
            else:
                raise r.Refused('watchdog_completion_deadline')


def install(router, upstream, preflight_deadline=None):
    canonical(router, upstream)
    if getattr(router, '_short_helpers', None) is not None:
        return router._short_helpers
    adapter = Helpers(router, upstream, preflight_deadline)
    router.rollback, router.restart = adapter.rollback, adapter.restart
    upstream.pristine_whitelist = adapter.pristine_whitelist
    upstream.restore_if_needed = adapter.restore_if_needed
    upstream.wait_child_ready = adapter.wait_child_ready
    upstream.local_connection, upstream.interrupt = adapter.local_connection, adapter.interrupt
    router._short_helpers = adapter
    return adapter
