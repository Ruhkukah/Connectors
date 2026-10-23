"""Reproduce the omitted-owner refusal without a live router or process."""
import unittest
from unittest.mock import Mock, patch
import ops


class Probe:
    def __init__(self, clients):
        self.clients = clients
        self.calls = []

    def inspect(self, config, owner_pid=None):
        self.calls.append((config, owner_pid))
        if len(self.clients) > (1 if owner_pid else 0):
            raise ops.Stop('other_router_clients')
        if owner_pid and self.clients != [owner_pid]:
            raise ops.Stop('router_client_not_test_owner')
        return {}, dict(client_count=len(self.clients), test_owner_verified=bool(owner_pid))


class OwnedInventory(unittest.TestCase):
    def test_disappearing_unrelated_process_repeats_full_inspection(self):
        probe=Mock();probe.inspect.side_effect=[FileNotFoundError(),({},dict(client_count=1))]
        with patch.object(ops,'process_identity',return_value=dict(pid=101,start_ticks=1)),patch.object(ops.time,'sleep'):
            self.assertEqual(ops.inspect_router(probe,'product','hash',101)[1]['client_count'],1)
        self.assertEqual(probe.inspect.call_count,2)
        self.assertTrue(all(c.kwargs['owner_pid']==101 for c in probe.inspect.call_args_list))

    def test_unstable_scan_stays_blocked(self):
        probe=Mock();probe.inspect.side_effect=FileNotFoundError()
        with patch.object(ops.time,'sleep'),self.assertRaisesRegex(ops.Stop,'router_inventory_unstable'):
            ops.inspect_router(probe,'product','hash')
        self.assertEqual(probe.inspect.call_count,3)

    def test_retry_does_not_hide_foreign_client_or_owner_change(self):
        probe=Mock();probe.inspect.side_effect=[FileNotFoundError(),ops.Stop('other_router_clients')]
        with patch.object(ops.time,'sleep'),self.assertRaisesRegex(ops.Stop,'other_router_clients'):
            ops.inspect_router(probe,'product','hash')
        probe.inspect.side_effect=[FileNotFoundError(),({},dict(client_count=1))]
        with patch.object(ops.time,'sleep'),patch.object(ops,'process_identity',side_effect=[dict(pid=101,start_ticks=1),dict(pid=101,start_ticks=2)]),self.assertRaisesRegex(ops.Stop,'router_inventory_owner_changed'):
            ops.inspect_router(probe,'product','hash',101)

    def test_product_pid_reaches_strict_inspection(self):
        probe = Probe([101])
        # This is the executed bootstrap's failing call, not a vendor failure.
        with self.assertRaisesRegex(ops.Stop, 'other_router_clients'):
            probe.inspect(dict(environment='test'))
        _, health = ops.inspect_router(probe, 'product', 'frozen_hash', 101)
        self.assertEqual(health['client_count'], 1)
        self.assertTrue(health['test_owner_verified'])
        self.assertEqual(probe.calls[-1], (dict(environment='test', binary='product', binary_sha256='frozen_hash'), 101))

    def test_real_competing_client_still_rejected(self):
        with self.assertRaisesRegex(ops.Stop, 'other_router_clients'):
            ops.inspect_router(Probe([101, 202]), 'product', 'frozen_hash', 101)

    def test_foreign_or_missing_owned_client_rejected(self):
        for clients in ([202], []):
            with self.subTest(clients=clients):
                with self.assertRaisesRegex(ops.Stop, 'router_client_not_test_owner'):
                    ops.inspect_router(Probe(clients), 'product', 'frozen_hash', 101)

    def test_zero_clients_required_without_owner(self):
        self.assertEqual(ops.inspect_router(Probe([]), 'product', 'frozen_hash')[1]['client_count'], 0)
        with self.assertRaisesRegex(ops.Stop, 'other_router_clients'):
            ops.inspect_router(Probe([101]), 'product', 'frozen_hash')


if __name__ == '__main__':
    unittest.main()
