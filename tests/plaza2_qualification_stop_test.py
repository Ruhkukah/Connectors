"""Send real POSIX signals while the subprocess is in a blocking vendor-style poll."""
import json
import os
import signal
import subprocess
import sys
import time

for signum in (signal.SIGTERM, signal.SIGINT):
    for scenario in ('idle', 'heavy', 'commit', 'internal'):
        with subprocess.Popen([sys.argv[1], scenario], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True) as child:
            assert child.stdout.readline().strip() == f'ENTER {scenario}'
            time.sleep(0.04)
            os.kill(child.pid, signum)
            stdout, stderr = child.communicate(timeout=5)
            result = json.loads(stdout)
            assert child.returncode == (3 if scenario == 'internal' else 0), (result, stderr)
            assert result['polls'] == result['teardown'] == 1, result
            assert result['failed'] == (scenario == 'internal'), result
            assert result['stop_observed'] == (scenario != 'internal'), result
            assert result['recovery_attempts'] == result['publisher_posts'] == 0, result
            assert (result['callbacks'] > 0) == (scenario == 'heavy'), result
            assert result['commits'] == (1 if scenario == 'commit' else 0), result
            print(signum.name, scenario, 'PASS', result)
