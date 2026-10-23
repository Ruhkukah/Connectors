"""Real frozen Probe composition over fake /proc; no native process or DNS."""
from dataclasses import replace
import hashlib
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock

import t0_context as t0

PROFILE = (b'[ROUTEINFO]\ndefault=spectra-t0.moex.com:53001\n'
           b'direct=spectra-t0.moex.com:53000\ndirect.2=spectra-t0.moex.com:53003\n'
           b'direct.3=spectra-t0.moex.com:53004\n')


def frozen(name):
    path = Path(__file__).with_name('fixtures') / (name + '.py.txt')
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    loader.exec_module(module)
    return module


class Context(unittest.TestCase):
    def setUp(self):
        old = {name: sys.modules.get(name) for name in ('router_recovery', 'upstream_recovery')}
        def restore():
            for name, module in old.items():
                if module is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = module
        self.addCleanup(restore)
        self.router, self.upstream = frozen('router_recovery'), frozen('upstream_recovery')
        self.temp = tempfile.TemporaryDirectory(prefix='t0-context-offline-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.work = self.root / 't0-private'
        self.work.mkdir(mode=0o700)
        self.exe = self.root / 'P2MQRouter64'
        self.exe.write_bytes(b'reviewed-router')
        self.product = self.root / 'moexctl'
        self.product.write_bytes(b'reviewed-native')
        self.profile = self.root / 'links_public.t0.ini'
        self.profile.write_bytes(PROFILE)
        self.config = self.work / 'router.ini'
        self.config.write_text('[P2MQRouter]\nport=4101\n[ROUTEINFO]\ntype=client\nconnections_ini=' +
                               str(self.profile) + '\n')
        self.config.chmod(0o600)
        self.proc = self.root / 'proc'
        (self.proc / 'net').mkdir(parents=True)
        self.rows = []
        self.context = t0.T0Context(self.exe, t0.sha(self.exe), self.work, self.config,
                                    t0.sha(self.config), self.profile, 100, '1000')
        self.process(100, self.exe, self.work, self.config, '1000', [1])
        self.row(4101, 0, '0A', 1)
        self.probe = t0.t0_scoped_probe(self.router, self.context, self.proc)

    def process(self, pid, executable, cwd, config, start, sockets=(), product=False):
        p = self.proc / str(pid)
        p.mkdir()
        (p / 'fd').mkdir()
        (p / 'exe').symlink_to(executable)
        (p / 'cwd').symlink_to(cwd)
        args = [str(executable), '--environment', 'test'] if product else [str(executable), '/ini:' + str(config)]
        (p / 'cmdline').write_bytes(b'\0'.join(os.fsencode(x) for x in args) + b'\0')
        (p / 'environ').write_bytes(b'')
        fields = ['0'] * 20
        fields[0], fields[19] = 'S', str(start)
        (p / 'stat').write_text(str(pid) + ' (a router (with spaces)) ' + ' '.join(fields))
        for inode in sockets:
            (p / 'fd' / str(inode)).symlink_to('socket:[' + str(inode) + ']')

    def row(self, local, peer, state, inode):
        self.rows.append(('0100007F:' + format(local, '04X'),
                          '0100007F:' + format(peer, '04X'), state, str(inode)))
        lines = ['header']
        for index, (loc, remote, status, ino) in enumerate(self.rows):
            lines.append(f'{index}: {loc} {remote} {status} 0:0 0:0 0 0 0 {ino}')
        (self.proc / 'net' / 'tcp').write_text('\n'.join(lines) + '\n')

    def inspect(self, owner=None):
        return self.probe.inspect(dict(environment='test', binary=str(self.product),
                                       binary_sha256=t0.sha(self.product)), owner)

    def whitelist(self, resolver=None, archived=PROFILE, clock=lambda: 1000.):
        wrapper = t0.t0_whitelist_factory(self.upstream, self.context, archived,
                     resolver or (lambda host, port: ['91.203.252.25']), now=clock)
        return wrapper(self.probe, self.inspect()[0], t0.T0_PROFILE_SHA256)

    def test_public_profile_is_the_exact_real_vendor_pin(self):
        self.assertEqual(hashlib.sha256(PROFILE).hexdigest(), t0.T0_PROFILE_SHA256)

    def test_exact_t0_launch_without_clients_passes(self):
        launch, health = self.inspect()
        self.assertEqual((launch.pid, launch.start), (100, '1000'))
        self.assertTrue(health['t0_scope_verified'])
        self.assertEqual(health['local_port'], 4101)
        self.assertEqual(health['client_count'], 0)

    def test_t1_in_another_scope_is_never_selected(self):
        other = self.root / 't1'
        other.mkdir()
        cfg = other / 'router.ini'
        cfg.write_text('t1 configuration untouched')
        self.process(200, self.exe, other, cfg, '2000', [9])
        self.row(4102, 0, '0A', 9)
        before = cfg.read_bytes()
        self.assertEqual(self.probe.owned_routers(), [(100, self.exe)])
        self.assertEqual(self.inspect()[0].pid, 100)
        self.assertEqual(cfg.read_bytes(), before)

    def test_isolated_42101_excludes_existing_t1_4101(self):
        self.config.write_text(self.config.read_text().replace('4101', '42101'))
        self.context = replace(self.context, port=42101, config_sha256=t0.sha(self.config))
        self.probe = t0.t0_scoped_probe(self.router, self.context, self.proc)
        self.rows = []
        self.row(42101, 0, '0A', 1)
        other = self.root / 't1'
        other.mkdir()
        self.process(200, self.exe, other, other / 'router.ini', '2000', [9])
        self.row(4101, 0, '0A', 9)
        self.assertEqual(self.inspect()[1]['client_count'], 0)
        self.assertEqual(self.probe.owned_routers(), [(100, self.exe)])

    def test_same_config_or_workdir_with_unknown_pid_refuses(self):
        self.process(200, self.exe, self.work, self.config, '2000')
        with self.assertRaisesRegex(t0.Refused, 't0_router_epoch_changed'):
            self.inspect()

    def test_foreign_workdir_taking_t0_port_refuses(self):
        other = self.root / 'unknown'
        other.mkdir()
        self.process(200, self.exe, other, other / 'router.ini', '2000', [9])
        self.row(4101, 0, '0A', 9)
        with self.assertRaisesRegex(t0.Refused, 't0_unknown_router_in_scope'):
            self.inspect()

    def test_reused_pid_is_not_accepted(self):
        p = self.proc / '100' / 'stat'
        p.write_text(p.read_text().replace('1000', '1001'))
        with self.assertRaisesRegex(t0.Refused, 't0_router_epoch_changed'):
            self.inspect()

    def test_config_tamper_refuses_even_if_no_router_selected(self):
        self.config.write_text(self.config.read_text() + '; tampered\n')
        with self.assertRaisesRegex(t0.Refused, 't0_config_hash_changed'):
            self.inspect()

    def test_profile_tamper_refuses(self):
        self.profile.write_bytes(PROFILE.replace(b't0', b't1'))
        with self.assertRaisesRegex(t0.Refused, 't0_profile_hash_changed'):
            self.inspect()

    def test_router_binary_tamper_refuses(self):
        self.exe.write_bytes(b'changed executable')
        with self.assertRaisesRegex(t0.Refused, 't0_router_binary_changed'):
            self.inspect()

    def test_private_config_permissions_required(self):
        self.config.chmod(0o644)
        with self.assertRaisesRegex(t0.Refused, 't0_config_not_private_owned'):
            self.inspect()

    def test_owned_native_client_passes_and_foreign_client_refuses(self):
        self.process(300, self.product, self.work, None, '3000', [3], product=True)
        (self.proc / '100' / 'fd' / '2').symlink_to('socket:[2]')
        self.row(4101, 60000, '01', 2)
        self.row(60000, 4101, '01', 3)
        self.assertEqual(self.inspect(300)[1]['client_count'], 1)
        (self.proc / '300' / 'fd' / '3').unlink()
        with self.assertRaisesRegex(t0.Refused, 't0_client_not_owned_product'):
            self.inspect(300)

    def test_no_owner_requires_zero_clients(self):
        (self.proc / '100' / 'fd' / '2').symlink_to('socket:[2]')
        self.row(4101, 60000, '01', 2)
        with self.assertRaisesRegex(t0.Refused, 't0_other_router_clients'):
            self.inspect()

    def test_whitelist_has_all_four_ports_and_fresh_dns_on_every_call(self):
        resolver = Mock(return_value=['91.203.252.25', '91.203.252.26'])
        white = self.whitelist(resolver)
        self.assertEqual(white.configured_count, 4)
        self.assertEqual(len(white.peers), 8)
        self.assertEqual(set(resolver.call_args_list[i].args for i in range(4)), t0.T0_ENDPOINTS)
        self.whitelist(resolver)
        self.assertEqual(resolver.call_count, 8)

    def test_archive_mismatch_refuses_before_dns(self):
        resolver = Mock()
        with self.assertRaisesRegex(t0.Refused, 't0_vendor_archive_profile_mismatch'):
            self.whitelist(resolver, archived=PROFILE + b'\n')
        resolver.assert_not_called()

    def test_t1_expected_pin_is_never_relabelled_t0(self):
        factory = t0.t0_whitelist_factory(self.upstream, self.context, PROFILE, Mock())
        with self.assertRaisesRegex(t0.Refused, 't0_expected_profile_pin_changed'):
            factory(self.probe, self.inspect()[0], 'ff64e85ac33958025c928f7cafd5da1804e35406bf3f016058f13e5826ed597a')

    def test_empty_loopback_and_slow_dns_refuse(self):
        for addresses, code in (([], 't0_dns_endpoint_empty'),
                                (['127.0.0.1'], 't0_dns_address_not_upstream')):
            with self.subTest(addresses=addresses), self.assertRaisesRegex(t0.Refused, code):
                self.whitelist(lambda host, port: addresses)
        clock = iter([1000., 1031.])
        with self.assertRaisesRegex(t0.Refused, 't0_dns_resolution_not_fresh'):
            self.whitelist(clock=lambda: next(clock))

    def test_dns_cannot_hide_configuration_change(self):
        def resolver(host, port):
            self.config.write_text(self.config.read_text() + '; during DNS\n')
            return ['91.203.252.25']
        with self.assertRaisesRegex(t0.Refused, 't0_config_hash_changed'):
            self.whitelist(resolver)


if __name__ == '__main__':
    unittest.main()
