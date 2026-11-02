"""Read-only T0 launch and endpoint proofs; this module never activates a job.

These factories are deliberately not installed into the frozen T1 runtime.
Every router epoch needs a new externally recorded PID/start-tick context. The
fault runtime's independent T1 constants/port checks still need composition
review before this can be used for a native continuation.
"""
from dataclasses import dataclass
import configparser
import hashlib
import ipaddress
import os
from pathlib import Path
import re
import time

T0_PROFILE_SHA256 = '7b84fa5d44118dc2ef6fe559174f27f78e362ec568f683ba6bef8f7b5276efe6'
T0_LOCAL_PORT = 4101
T0_HOST = 'spectra-t0.moex.com'
T0_ENDPOINTS = frozenset((T0_HOST, port) for port in (53000, 53001, 53003, 53004))


class Refused(Exception):
    pass


def require(condition, code):
    if not condition:
        raise Refused(code)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


@dataclass(frozen=True)
class T0Context:
    executable: Path
    executable_sha256: str
    workdir: Path
    config: Path
    config_sha256: str
    profile: Path
    router_pid: int
    router_start: str
    port: int = T0_LOCAL_PORT


def config_profile(context):
    """Only explicit paths are supported; no credential/environment expansion."""
    require(context.port in (4101, 42101), 't0_local_port_not_reviewed')
    require(type(context.router_pid) is int and context.router_pid > 0 and
            str(context.router_start).isdigit(), 't0_router_epoch_missing')
    directory = Path(context.workdir).resolve(strict=True)
    config = Path(context.config).resolve(strict=True)
    profile = Path(context.profile).resolve(strict=True)
    require(config.parent == directory, 't0_config_outside_private_workdir')
    require(directory.stat().st_uid == os.getuid() and
            directory.stat().st_mode & 0o077 == 0, 't0_workdir_not_private_owned')
    require(config.stat().st_uid == os.getuid() and
            config.stat().st_mode & 0o077 == 0, 't0_config_not_private_owned')
    require(sha(config) == context.config_sha256, 't0_config_hash_changed')
    require(sha(context.executable) == context.executable_sha256, 't0_router_binary_changed')
    ini = configparser.ConfigParser(interpolation=None)
    ini.read_string(config.read_text())
    require(ini.getint('P2MQRouter', 'port') == context.port, 't0_config_port_changed')
    require(ini.get('ROUTEINFO', 'type').strip().lower() == 'client', 't0_route_not_client')
    raw = ini.get('ROUTEINFO', 'connections_ini').strip().strip('"')
    # A sealed T0 configuration uses a literal absolute file name. Refuse legacy
    # pipe grammar and expansion instead of accidentally selecting another site.
    require('|' not in raw and '$' not in raw and Path(raw).is_absolute(),
            't0_route_profile_path_not_explicit')
    require(Path(raw).resolve(strict=True) == profile, 't0_route_profile_changed')
    require(sha(profile) == T0_PROFILE_SHA256, 't0_profile_hash_changed')
    return profile


def _argv_config(argv, cwd):
    matches = []
    for value in argv:
        token = os.fsdecode(value)
        if token.lower().startswith('/ini:'):
            path = Path(token[5:])
            matches.append((path if path.is_absolute() else cwd / path).resolve())
    return matches


def t0_scoped_probe(router_module, context, proc=Path('/proc')):
    """Return a strict, read-only Probe for one externally pinned T0 epoch.

    T1 routers on a different configuration, cwd and port are excluded. A router
    touching any T0 scope marker must match every marker and the exact epoch;
    unknown or reused PIDs refuse the inventory rather than disappearing.
    """
    class ScopedProbe(router_module.Probe):
        def _identity(self, pid, executable):
            return super().identity(pid, executable)

        def owned_routers(self):
            config_profile(context)
            expected = Path(context.executable).resolve(strict=True)
            result = []
            for pid, executable in super().owned_routers():
                process = self.proc / str(pid)
                start = self._identity(pid, executable)
                cwd = (process / 'cwd').resolve(strict=True)
                argv = [part for part in (process / 'cmdline').read_bytes().split(b'\0') if part]
                configs = _argv_config(argv, cwd)
                owned_sockets = self.sockets(pid)
                touches_port = any(row[2] == '0A' and row[3] in owned_sockets and
                                   int(row[0].split(':')[-1], 16) == context.port
                                   for row in self.tcp())
                touches_scope = (cwd == Path(context.workdir).resolve() or
                                 Path(context.config).resolve() in configs or touches_port)
                require(self._identity(pid, executable) == start, 't0_router_changed_during_inventory')
                if not touches_scope:
                    continue
                require(executable == expected and cwd == Path(context.workdir).resolve() and
                        configs == [Path(context.config).resolve()], 't0_unknown_router_in_scope')
                require(pid == context.router_pid and start == str(context.router_start),
                        't0_router_epoch_changed')
                result.append((pid, executable))
            require(len(result) <= 1, 't0_router_scope_not_unique')
            return result

        def identity(self, pid, executable):
            start = self._identity(pid, executable)
            if pid == context.router_pid:
                require(Path(executable).resolve(strict=True) == self.executable and
                        start == str(context.router_start), 't0_router_epoch_changed')
            return start

        def clients(self, router_pid, owner_pid=None, exact=True):
            before = self.identity(router_pid, self.executable)
            require(router_pid == context.router_pid, 't0_router_pid_outside_scope')
            owner_start = None
            owner_executable = None
            if owner_pid:
                owner_executable = (self.proc / str(owner_pid) / 'exe').resolve(strict=True)
                owner_start = self._identity(owner_pid, owner_executable)
            rows, sockets = self.tcp(), self.sockets(router_pid)
            servers = [row for row in rows if int(row[0].split(':')[-1], 16) == context.port]
            listeners = [row for row in servers if row[2] == '0A']
            require(len(listeners) == 1 and listeners[0][3] in sockets,
                    't0_listener_not_unique_owned')
            established = [row for row in servers if row[2] == '01']
            require(len(established) <= (1 if owner_pid else 0), 't0_other_router_clients')
            require(not (exact and owner_pid) or len(established) == 1,
                    't0_owner_connection_missing')
            if established:
                owner_sockets = self.sockets(owner_pid)
                for local, peer, _, inode in established:
                    paired = [row for row in rows if row[:3] == (peer, local, '01')]
                    require(inode in sockets and len(paired) == 1 and paired[0][3] in owner_sockets,
                            't0_client_not_owned_product')
            require(self.identity(router_pid, self.executable) == before, 't0_router_epoch_changed')
            if owner_pid:
                require(self._identity(owner_pid, owner_executable) == owner_start,
                        't0_product_epoch_changed')
            return len(established)

        def inspect(self, config, owner_pid=None):
            config_profile(context)
            launch, health = super().inspect(config, owner_pid)
            require(launch.pid == context.router_pid and launch.start == str(context.router_start) and
                    launch.cwd == Path(context.workdir).resolve() and
                    _argv_config(launch.argv, launch.cwd) == [Path(context.config).resolve()],
                    't0_launch_context_changed')
            config_profile(context)
            return launch, dict(health, t0_scope_verified=True, local_port=context.port)

    return ScopedProbe(context.executable, context.executable_sha256, Path(proc))


def t0_whitelist_factory(upstream_module, context, archived_profile, resolver, now=time.time):
    """Build a read-only whitelist function; resolver must do fresh DNS per call.

    ``archived_profile`` is the bytes from the verified vendor archive. The
    installer must separately seal that archive's SHA and extraction receipt.
    No subprocess, socket, launch, file mutation or cached DNS is used here.
    """
    def pristine_whitelist(probe, launch, expected_sha, vendor=None):
        del vendor
        require(expected_sha == T0_PROFILE_SHA256, 't0_expected_profile_pin_changed')
        require(probe.executable == Path(context.executable).resolve() and
                launch.pid == context.router_pid and launch.start == str(context.router_start) and
                launch.cwd == Path(context.workdir).resolve() and
                _argv_config(launch.argv, launch.cwd) == [Path(context.config).resolve()],
                't0_whitelist_launch_context_changed')
        profile = config_profile(context)
        raw = profile.read_bytes()
        require(raw == archived_profile and hashlib.sha256(archived_profile).hexdigest() ==
                T0_PROFILE_SHA256, 't0_vendor_archive_profile_mismatch')
        names = upstream_module.endpoint_list(raw)
        require(names == T0_ENDPOINTS, 't0_exact_four_endpoints_changed')
        started = now()
        peers = set()
        for host, port in sorted(names):
            addresses = resolver(host, port)
            require(addresses, 't0_dns_endpoint_empty')
            for address in addresses:
                ip = ipaddress.ip_address(address)
                if isinstance(ip, ipaddress.IPv6Address) and ip.ipv4_mapped:
                    ip = ip.ipv4_mapped
                require(not (ip.is_loopback or ip.is_unspecified or ip.is_multicast or
                             ip.is_link_local), 't0_dns_address_not_upstream')
                peers.add((str(ip), port))
        require(0 <= now() - started <= 30, 't0_dns_resolution_not_fresh')
        config_profile(context)
        require(profile.read_bytes() == raw, 't0_profile_changed_during_dns')
        return upstream_module.Whitelist(frozenset(peers), T0_PROFILE_SHA256, len(names))
    return pristine_whitelist
