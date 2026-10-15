"""Oct16 scoped adapter for the reviewed interruption core. Import is inert.

Only build_faultcase(...)(...) is active; it neither prepares nor starts a
product, old guardian or router. The root supervisor retains execution ownership.
"""
from pathlib import Path
import datetime as dt
import fcntl
import json
import os
import re
import stat
import time

DATE = '2026-10-16'
MSK = dt.timezone(dt.timedelta(hours=3))


def epoch(hour, minute=0):
    return dt.datetime(2026, 10, 16, hour, minute, tzinfo=MSK).timestamp()


def authorized(core, supervisor, cfg, now=None):
    now = time.time() if now is None else now
    core.require(cfg.get('authorized_date') == DATE and
                 dt.datetime.fromtimestamp(now, MSK).date().isoformat() == DATE,
                 'fault_bridge_authorized_date_changed')
    core.require(cfg.get('environment') == 'test' and cfg.get('owner_authorized') is True and
                 cfg.get('timer_paused') is True and cfg.get('one_passive_order_authorized') is True and
                 cfg.get('skip_firewall') is True and cfg.get('max_postday_seconds') == 7200,
                 'fault_bridge_owner_scope_missing')
    core.require(epoch(10) <= now < cfg['entry_deadline_utc'] == epoch(10, 45) and
                 cfg['cleanup_deadline_utc'] == epoch(11), 'fault_bridge_short_deadline_invalid')
    core.require(cfg['source_git_sha'] == supervisor.rt.HEAD and
                 cfg['binary_sha256'] == supervisor.rt.BINARY and
                 cfg['router_helper_sha256'] == core.ROUTER_HELPER_SHA and
                 cfg['upstream_helper_sha256'] == core.UPSTREAM_HELPER_SHA,
                 'fault_bridge_protected_pin_changed')


def transient_fd(error):
    return (isinstance(error, FileNotFoundError) and error.filename is not None and
            re.fullmatch(r'/proc/[1-9][0-9]*/fd/[0-9]+', os.fsdecode(error.filename)) is not None)


class BoundedProbe:
    """Restart a whole inspection, only for a disappearing /proc/PD/fd/FD."""
    def __init__(self, raw, ops, meta, require, sha):
        self.raw, self.ops, self.meta, self.require, self.sha = raw, ops, meta, require, sha

    def __getattr__(self, name):
        return getattr(self.raw, name)

    def router_identity(self):
        rows = list(self.raw.owned_routers())
        self.require(len(rows) == 1, 'fault_bridge_router_ambiguous_or_absent')
        row = rows[0]
        pid = row[0] if isinstance(row, (tuple, list)) else row
        if isinstance(row, (tuple, list)):
            self.require(len(row) == 2 and Path(row[1]).resolve() == Path(self.raw.executable).resolve(),
                         'fault_bridge_router_executable_changed')
        identity = self.ops.process_identity(pid)
        self.require(Path(identity['executable']).resolve() == Path(self.raw.executable).resolve() and
                     self.sha(self.raw.executable) == self.raw.expected_sha,
                     'fault_bridge_router_binary_changed')
        return identity

    def identities(self, router):
        self.require(self.ops.process_identity(self.meta['identity']['pid']) == self.meta['identity'],
                     'fault_bridge_product_identity_changed')
        self.require(self.sha(self.meta['binary']) == self.meta['binary_sha256'] and
                     Path(self.meta['identity']['executable']).resolve() == Path(self.meta['binary']).resolve(),
                     'fault_bridge_product_binary_changed')
        self.require(self.router_identity() == router, 'fault_bridge_router_identity_changed')

    def scan(self, operation):
        router = self.router_identity()
        for attempt in range(3):
            self.identities(router)
            try:
                result = operation()
            except FileNotFoundError as error:
                self.identities(router)
                if not transient_fd(error):
                    raise
                self.require(attempt < 2, 'fault_bridge_proc_fd_race_exhausted')
                time.sleep(.02)
                continue
            except BaseException:
                self.identities(router)
                raise
            self.identities(router)
            return result

    def inspect(self, *args, **kwargs):
        return self.scan(lambda: self.raw.inspect(*args, **kwargs))



class NativeKillGuard:
    """Observe actual operator/native guard records; they are not replication transactions."""
    def __init__(self, path, require, touch):
        self.path, self.require, self.touch = Path(path), require, touch
        item = self.path.stat()
        self.identity, self.offset = (item.st_dev, item.st_ino), 0
        self.request = self.enabled_at = None

    def update(self):
        with self.path.open('rb') as source:
            item = os.fstat(source.fileno())
            self.require((item.st_dev, item.st_ino) == self.identity and item.st_size >= self.offset,
                         'fault_bridge_guard_journal_changed')
            source.seek(self.offset)
            while source.tell() < item.st_size:
                begin = source.tell()
                line = source.readline()
                if not line.endswith(b'\n'):
                    break
                event = json.loads(line)
                self.offset = source.tell()
                name, data = event.get('event'), event.get('data', {})
                if name == 'startup':
                    self.request = self.enabled_at = None
                elif name == 'operator_input' and str(data.get('line', '')).startswith('kill '):
                    self.enabled_at = None
                    self.request = begin if (data.get('line') == 'kill on' and
                                              data.get('channel') == 'command_socket') else None
                elif name == 'kill_switch':
                    self.enabled_at = begin if self.request is not None and data.get('enabled') is True else None

    def prove(self):
        until = time.monotonic() + 2
        while True:
            self.update()
            if self.request is not None and self.enabled_at is not None:
                return dict(operator_input_offset=self.request, native_enabled_offset=self.enabled_at,
                            proof='native_operator_kill_on_then_kill_switch_enabled')
            self.require(time.monotonic() < until, 'fault_bridge_native_kill_guard_unproved')
            self.touch()
            time.sleep(.02)  # Journal catch-up only; never sends/retries a command.


class GuardedStatusProxy:
    """Narrow old-core schema adapter. Real product status/ownership stays unchanged."""
    def __init__(self, product, guard):
        self.original, self.guard = product, guard

    def __getattr__(self, name):
        return getattr(self.original, name)

    def wait_status(self, *args, **kwargs):
        # The forked fault worker replaces proxy.child with OwnedProcess.
        # Bind inherited waiting methods to this proxy, never parent Popen.
        return type(self.original).wait_status(self, *args, **kwargs)

    def wait_ready(self, *args, **kwargs):
        return type(self.original).wait_ready(self, *args, **kwargs)

    def status(self):
        value = self.original.status()
        self.guard.prove()
        # Native status has no live kill_switch member. This derived field is
        # supplied solely to the old core after its actual journal proof.
        return dict(value, configuration=dict(value['configuration'],
                    risk=dict(value['configuration']['risk'], kill_switch=True)))


def build_faultcase(supervisor, core, router, upstream):
    """Return the ShortDriver callback; construction performs no native action."""
    cfg = dict(supervisor.cfg)
    cfg['entry_deadline_utc'] = cfg.get('entry_deadline_utc', cfg.get('hard_entry_deadline_utc'))
    cfg['cleanup_deadline_utc'] = cfg.get('cleanup_deadline_utc', cfg.get('hard_cleanup_deadline_utc'))

    def faultcase(product, name, kind, seq, client, target):
        authorized(core, supervisor, cfg)
        core.require(kind in ('router', 'upstream') and re.fullmatch(r'[A-Za-z0-9_-]{1,100}', name),
                     'fault_bridge_case_scope_invalid')
        core.require(product is supervisor.product and supervisor.meta is not None and
                     product.child.pid == supervisor.meta['identity']['pid'], 'fault_bridge_product_not_root_owned')
        core.require(target in supervisor.targets and target['symbol'] in {'ALRS-12.26', 'CNY-12.26'},
                     'fault_bridge_target_outside_scope')
        # Parent verified modules in its immutable manifest; independently match
        # the actual helper bytes before allowing either reviewed fault path.
        core.require(core.sha(router.__file__) == cfg['router_helper_sha256'] and
                     core.sha(upstream.__file__) == cfg['upstream_helper_sha256'],
                     'fault_bridge_helper_bytes_changed')
        c = core.Controller(supervisor.work, cfg)
        kill_guard = NativeKillGuard(product.work / 'interaction.ndjson', core.require, supervisor.tick)
        c.product = GuardedStatusProxy(product, kill_guard)
        c.product_identity = dict(supervisor.meta['identity'])
        c.binary, c.environment, c.targets = Path(cfg['binary']), supervisor.environment, supervisor.targets
        c.ops, c.router, c.upstream = supervisor.ops, router, upstream
        c.sequence, c.prices = supervisor.sequence, supervisor.prices
        c.budget = lambda: supervisor.rt.budget(supervisor.meta)
        c.initial_highwaters = core.checkpoint_highwaters(c.environment['state'])
        raw, strict_guard, _ = supervisor.rt._probe()
        c.probe = BoundedProbe(raw, c.ops, supervisor.meta, core.require, core.sha)
        def initial_scan():
            launch, _ = raw.inspect(dict(environment='test', binary=str(c.binary),
                        binary_sha256=cfg['binary_sha256']), owner_pid=product.child.pid)
            white = upstream.pristine_whitelist(c.probe, launch, core.T1_LINKS_SHA)
            peers = {row.peer for row in strict_guard.strict_upstreams(upstream, c.probe, launch.pid, white)}
            return launch, peers
        c.original_router, c.initial_peers = c.probe.scan(initial_scan)
        core.require(len(c.initial_peers) == 4, 'fault_bridge_four_pristine_peers_unproved')
        c.passive_target, c.passive_cid = target, client
        c.touch = supervisor.tick  # No inventory check while the expected outage is active.

        def inspect(exact=True):
            supervisor.rt.verify_owner(supervisor.meta)
            def full_scan():
                launch, health = c.probe.raw.inspect(dict(environment='test', binary=str(c.binary),
                        binary_sha256=cfg['binary_sha256']), owner_pid=product.child.pid if exact else None)
                core.same_launch(c.original_router, launch)
                whitelist = upstream.pristine_whitelist(c.probe, launch, core.T1_LINKS_SHA)
                peers = {row.peer for row in strict_guard.strict_upstreams(upstream, c.probe, launch.pid, whitelist)}
                core.require(peers == c.initial_peers and health['client_count'] == 1,
                             'fault_bridge_restored_peers_or_sole_client_unproved')
                return launch
            return c.probe.scan(full_scan)
        c.inspect = inspect

        def entry_gate():
            authorized(core, supervisor, cfg)
            inspect(); core.require(c.budget() >= 7000, 'fault_bridge_identifier_budget_insufficient')
            core.require(cfg['cleanup_deadline_utc'] - time.time() >= 150,
                         'fault_bridge_cleanup_budget_insufficient')
            state = c.full_state(entry=True)  # Exactly one fresh product status grants readiness.
            supervisor.rt.configuration_gate(state, c.targets, cfg)
            c.one_working(state)
            for current in c.targets:
                bid = c.prices.current_reference(state, current)
                core.require(current['quote_cap_scaled'] <= 2 * bid,
                             'fault_bridge_frozen_cap_exceeds_fresh_bid')
            return state
        c.entry_gate = entry_gate
        # Acquire the canonical cross-controller lock, never a copied job lock.
        lock = Path(cfg['fault_lock'])
        q = (Path.home() / 'moex/qualification').resolve(strict=True)
        core.require(lock.is_absolute() and lock.parent.resolve(strict=True).is_relative_to(q) and
                     not lock.is_symlink() and lock.parent.stat().st_uid == os.getuid() and
                     lock.name == 'router-fault.lock', 'fault_bridge_global_lock_path_unproved')
        fd = os.open(lock, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        try:
            info = os.fstat(fd)
            core.require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid(),
                         'fault_bridge_global_lock_identity_unproved')
        except BaseException:
            os.close(fd)
            raise
        c.fault_lock = os.fdopen(fd, 'a')
        c.guard_control = os.open(os.devnull, os.O_RDONLY)  # Harmless child-close FD; root guardian is external.
        old_authorized, old_source = core.authorized, core.PINNED_SOURCE
        try:
            fcntl.flock(c.fault_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            core.authorized = lambda value, now=None: authorized(core, supervisor, value, now)
            core.PINNED_SOURCE = cfg['source_git_sha']
            c.working_fault(name, kind)  # Includes one bounded kill-guarded CLI refusal and native recovery proof.
            core.require(c.fault_worker is None and len(c.public['cases']) == 1,
                         'fault_bridge_worker_or_case_incomplete')
            result = dict(c.public['cases'][0])
            result['kill_guard_provenance'] = kill_guard.prove()
            result['local_refusal']['causal_scope'] = 'kill_switch_guard_during_observed_outage_not_transport_causality'
            supervisor.publish({'last_fault_case': result})
            return result
        finally:
            core.authorized, core.PINNED_SOURCE = old_authorized, old_source
            if c.fault_worker is not None:
                try:
                    c.wait_fault_worker()
                except BaseException:
                    supervisor.pending_fault_controller = c
                    raise  # Preserve its lock/control FD while independent restoration remains pending.
            os.close(c.guard_control)
            c.fault_lock.close()
    return faultcase
