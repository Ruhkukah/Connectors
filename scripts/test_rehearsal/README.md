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
The original deployed bundles remain unchanged. Subsequent corrections in this
review copy are recorded separately in PR #68 with their regression evidence.
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

Post-run correction: the protected inventory/isolation adapter must pass the
expected product PID to `ops.inspect_router()`. A zero-client inspection after
startup rejects the product's own socket and incorrectly selects the emergency
SIGKILL shutdown path. The inspection must still verify the TEST binary and
paired socket ownership, reject competing clients, and use zero-client mode
before startup/after shutdown. Historical deployed sources stay unchanged;
using this corrected helper does not authorize a repeat session.

The startup wait retries only a read-only status request whose CLI reports
`cannot connect to command socket`, within the existing deadline and while the
child is alive. It does not retry a trading command, a missing response,
malformed JSON or a product safety alert. Cleanup also waits for a responsive
status socket before sending its first command.

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

Current-price cap correction: `session_terms.reference_price_scaled` is the
REFDATA settlement price used for native price-band arithmetic. It must not
stand in for an executable current price. Both it and AGGR prices use five
fractional digits. The supervisor first captures a fresh two-sided AGGR book
through the native read-only product, with no trading publisher, and validates
identity, session, grid and bounds against committed REFDATA. The fixed quote
cap is twice the best bid, the conservative side of that book. It records the
settlement price and current quote anchor separately. All eight trading streams,
flatness, reservations and effective configuration must then pass before entry.
The cap is never increased during that session; each request also stays below
twice the fresh best bid. A two-lot buy at the ask can therefore be refused even
when a one-lot IOC is allowed. Quantity, position, rate and ownership limits are
unchanged. These are outstanding-order quote units, not cash margin or converted
contract currency value.

Closed-session closeout correction: a current reconstructed empty order scope
needs no exchange cancellation. Missing history or pending cancellation still
requires the normal risk-reduction path. Final cleanup publishes `closing`,
with a fresh lease bounded by 240 seconds after the planned end. The guardian
respects that lease and cleanup lock; a stale heartbeat or expired lease still
triggers intervention. `complete` is published only with the terminal closeout
receipt after cancellation/flatness, product exit and client inventory checks.
