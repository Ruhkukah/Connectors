"""Regression for status racing stale socket removal during process restart."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import ops


class StartupSocket(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.work = Path(self.tmp.name)
        (self.work / 'cmd.sock').touch()
        self.p = ops.Product(self.work, sys.executable, {}, [], [])
        self.p.child = Mock()
        self.p.child.poll.return_value = None
        self.now = ops.START + 1

    def tearDown(self):
        self.tmp.cleanup()

    def sleep(self, seconds):
        self.now += seconds

    def run_sequence(self, responses, until=None):
        with patch.object(ops.time, 'time', side_effect=lambda: self.now), patch.object(ops.time, 'sleep', side_effect=self.sleep), patch.object(ops.subprocess, 'run', side_effect=responses) as run, patch.object(ops, 'ready', return_value=True):
            result = self.p.wait_ready(until or self.now + 1)
            self.assertTrue(all(call.args[0][-1] == 'status' for call in run.call_args_list))
            return result, run.call_count

    def unavailable(self):
        return subprocess.CompletedProcess([], 2, b'', b'moexctl: cannot connect to command socket\n')

    def test_stale_socket_then_ready_recovers(self):
        result, calls = self.run_sequence([self.unavailable(), subprocess.CompletedProcess([], 0, b'{"queued":0}', b'')])
        self.assertEqual(result['queued'], 0)
        self.assertEqual(calls, 2)

    def test_unavailability_is_bounded(self):
        with patch.object(ops.time, 'time', side_effect=lambda: self.now), patch.object(ops.time, 'sleep', side_effect=self.sleep), patch.object(ops.subprocess, 'run', return_value=self.unavailable()):
            with self.assertRaisesRegex(ops.Stop, 'status_socket_startup_deadline'):
                self.p.wait_status(self.now + .11)
        self.assertLess(self.now, ops.START + 1.2)

    def test_process_exit_is_not_retried(self):
        self.p.child.poll.return_value = 3
        with patch.object(ops.time, 'time', return_value=self.now), patch.object(ops.subprocess, 'run') as run:
            with self.assertRaisesRegex(ops.Stop, 'product_exited'):
                self.p.wait_status(self.now + 1)
            run.assert_not_called()

    def test_other_status_failure_is_not_retried(self):
        with self.assertRaisesRegex(ops.Stop, 'product_command_refused_or_uncertain'):
            self.run_sequence([subprocess.CompletedProcess([], 2, b'', b'command socket response missing')])

    def test_trading_command_is_never_reclassified_or_retried(self):
        with patch.object(ops.time, 'time', return_value=self.now), patch.object(ops.subprocess, 'run', return_value=self.unavailable()) as run:
            with self.assertRaises(ops.Stop) as error:
                self.p.command('place uncertain 1 buy 1 1 day')
            self.assertNotIsInstance(error.exception, ops.StatusSocketUnavailable)
            self.assertEqual(run.call_count, 1)

    def test_successful_status_safety_alert_still_stops(self):
        with self.assertRaisesRegex(ops.Stop, 'product_safety_alert'):
            self.run_sequence([self.unavailable(), subprocess.CompletedProcess([], 0, b'{"journal_failed":true}', b'')])


if __name__ == '__main__':
    unittest.main()
