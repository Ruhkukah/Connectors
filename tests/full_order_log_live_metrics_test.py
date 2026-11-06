import importlib.util
from pathlib import Path
import tempfile
import json
import unittest

script = Path(__file__).parent / 'plaza2_cgate/full_order_log_live_metrics.py'
spec = importlib.util.spec_from_file_location('metrics', script)
metrics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metrics)


class MetricsTest(unittest.TestCase):
    def test_cpu_identity_and_units(self):
        old = {'start_ticks': 100, 'cpu_ticks': 200}
        new = {'start_ticks': 100, 'cpu_ticks': 250}
        self.assertEqual(metrics.cpu_percent(old, new, 2, 100), 25.0)
        with self.assertRaises(RuntimeError):
            metrics.cpu_percent(old, {'start_ticks': 101, 'cpu_ticks': 300}, 2, 100)

    def test_missing_stale_and_wrong_process_p99_are_unavailable(self):
        self.assertFalse(metrics.trading_metric(None, 42, 100)['available'])
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / 'metric.json'
            value = {'pid': 42, 'metric_kind': 'owner_defined_trading_callback', 'p99_ns': 1200,
                     'sample_count': 100, 'observed_unix_ns': 100, 'password': 'must-not-survive'}
            p.write_text(json.dumps(value))
            result = metrics.trading_metric(p, 42, 101)
            self.assertTrue(result['available'])
            self.assertNotIn('password', result)
            self.assertFalse(metrics.trading_metric(p, 43, 101)['available'])
            self.assertFalse(metrics.trading_metric(p, 42, 31_000_000_101)['available'])
            value['sample_count'] = 0
            p.write_text(json.dumps(value))
            self.assertFalse(metrics.trading_metric(p, 42, 101)['available'])


if __name__ == '__main__':
    unittest.main()
