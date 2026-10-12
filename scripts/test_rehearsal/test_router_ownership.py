"""Reproduce the omitted-owner refusal without a live router or process."""
import unittest
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
