# September 11 ConnectionOpen recovery policy

Base: corrected PR #53, `07460fc06562ebad395530aaceb628168c5557a2`.
Historical live source: PR #54, `58fade62e0ffd454547091b1934a342430aa8581`.
Its `ZERO_ORDER_ROUTER_RECOVERY_FAIL` remains unchanged. This is offline
policy validation, not a new live qualification result.

## Rule and invariant

Only the existing supervisor's Recovering branch can retry ConnectionOpen
INVALIDARGUMENT (131073). Entry to that branch is exclusively through the
existing accepted transport-loss policy. Initial bootstrap never enters it.
The fresh connection create must have succeeded, and the same operational
lifetime must previously have completed create plus open with the identical
connection identity. Callback, private decoder, reply and state-query errors
retain fatal precedence. Unsupported, incorrect-state and unknown open results
remain fatal. Other CGate APIs do not receive this exception.

The private, length-framed SHA-256 identity covers the exact rendered connection
settings (including protocol, endpoint, port and object/app parameters), open
settings, rendered/resolved environment settings, endpoint host, credential and
software-key values, runtime library path/hash, scheme hash, and INI bytes.
The host's configuration is constructor-owned and has no mutation API. External
secret/config/runtime changes are checked again at bootstrap. A mismatch fails
before opening resources; it cannot establish a new successful identity during
recovery. Public stop/start clears the proof. No digest or secret-bearing text
is exported. A digest of credentials is not a privacy guarantee.

Hashing and INI reading occur only at bootstrap, never in normal polling.
ConnectionOpen INTERNAL retains the existing bounded rule. Neither allowed
result resets the episode deadline. Retry spacing is measured from completed
attempt teardown, at least the configured interval (minimum one second).
An expired deadline fails with the last cause; the first process cause remains
separate. Vendor log text is never parsed by the recovery algorithm.

## Regression evidence

`regression.json` records the same new test object linked against the unchanged
PR #53 production archives (exit 1), then against the new policy (exit 0).
Both expose first cause 131072 and current cause 131073. The negative control
is deliberately failing and is not part of the passing CTest count.

The extended connector-host test covers:

- September 11: Ready, process INTERNAL with connection ERROR, fresh create,
  open INVALIDARGUMENT, Recovering rather than Failed.
- Two unsuccessful opens then third success, for INTERNAL and INVALIDARGUMENT:
  attempts 3, generation advances only after successful bootstrap, fresh POS
  LifeNum 8/revision 91 anchors TRADE, private/AGGR/publisher/reply readiness.
- Both open errors exhaust the original deadline with two attempts and distinct
  first/current causes; idle polls cannot generate additional attempts.
- Initial INVALIDARGUMENT and a second operational lifetime remain fatal with
  zero recovery attempts. Changed credentials and changed INI content fail
  before open. Non-open bootstrap INTERNAL/INVALIDARGUMENT remain fatal.
- Existing three 1,000-TIMEOUT scenarios, fatal callback/decode/state-query
  precedence, active-order no-resend, account/terms and shutdown tests remain.

The existing fake runtime numeric-result controls express the live signature;
no new fake protocol or production vendor-log parser was necessary. Existing
order lifecycle tests use fake publisher calls. All live orders/posts remain zero.

## Validation and operational boundary

Local macOS arm64 Release: 179/179 CTests passed.
Local macOS arm64 ASan/UBSan: 126/126 CTests passed (LeakSanitizer is unsupported
on macOS). Native Linux CI results are recorded in the PR receipt.
Linux CI uses native GitHub-hosted x86-64 Ubuntu runners; it supplies both the
native x86-64 Release receipt and CI Release receipt, not two independent runs.
The workflow includes all preflight tests for this branch, plus the existing
ASan/UBSan/LeakSanitizer component suite. Sanitizer timings are not benchmarks.

The September 11 heartbeat was deleted. No new wake-up or dated harness was
created. Next date, MOEX availability check and harness preparation follow
review. No VPS mutation, T1 connection, deployment, exchange order or merge.
Business semantics, historical evidence and tested PR #53/#54 heads are unchanged.
