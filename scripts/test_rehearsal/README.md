# Historical TEST rehearsal orchestration

These sources expose the CLI sequencing, supervisor, bounded cleanup and
independent guardian prepared for the authorized 12 October 2026 one-shot run.
They do not change the connector. The historical production head is
`f849e05785e76de1d6a1c04fc879dea8a15829f2`; a later scripts-only commit is not a
new native qualification result. Actual results and private-bundle provenance
belong in PR #68's description, not in repository evidence files.

`worker.py` retains the original dated start/claim, session schedule, scenario
ordering, refusal handling and shutdown escalation. Its original date is past
after that run; this publication is not an armed job or permission to repeat it.
The source deliberately retains the original failure-reporting and signal
behavior for review rather than silently correcting the executed harness.
`guardian.py` watches the owned supervisor and product identities and performs
bounded product-mediated cleanup. Guardian completion alone proves no trading
or certification success.

`sequence.py` and `scenarios.py` perform passive cycles, concurrent cancellation,
IOC/fill accounting, restart checks and a genuine partial-fill-before-Move
check through supplied product CLI/status callables. They never send via a
separate SDK publisher, retry an uncertain Add or force a partial-fill result.
The worker intentionally excludes router/upstream fault injection and marks
MOEX-coordinated scenarios unobserved.

## Protected deployment boundary

Private bootstrap/coverage/reconstruction implementations, SSH launchers,
credentials, raw rows/logs, argument lists and state files are excluded.
`deployment_adapter.py.example` documents their interfaces without supplying
live settings. The actual protected adapter must independently verify the
exact native binary, protected configuration, original router identity,
pristine upstream whitelist, exclusive client ownership and assigned ID budget.
It must preserve the original native reservation-coverage proof and negative
control. There is no default adapter or work directory.

The review copy replaces private bootstrap imports and fixed deployment paths
with `deployment.load()` and protected hooks. The assigned ext-id range comes
from the protected adapter; `ops.capacity()` receives its bounds explicitly.
Test imports and temporary directories are portable; synthetic ID examples
replace the deployed assignment. Date, approved instruments, risk gates, CLI
scenario logic and cleanup/guardian sequencing otherwise remain as prepared.
These adapter changes have offline tests, not native equivalence qualification.

Adapter selection requires both `MOEX_REHEARSAL_ADAPTER` (an absolute protected
Python file) and `MOEX_REHEARSAL_ADAPTER_SHA256`. Adapter code is trusted executable
code, so its hash must come from independently reviewed protected deployment
settings. The existing work directory must be outside the repository. Do not
copy real settings into this tree, use this example to start another session,
or reset/widen persistent order identifiers.

## Offline checks

Run from the repository root:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/test_rehearsal -p 'test_*.py'
```

The original 34 safeguard tests use synthetic CLI/status and journal fixtures;
additional adapter tests verify fail-closed configuration selection. These
checks require neither TEST access nor a publisher and make no certification
claim. Linux is required for a real supervisor/guardian because process identity
uses `/proc` and cleanup locking uses `flock`.
