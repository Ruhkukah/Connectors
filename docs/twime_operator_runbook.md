# TWIME Operator Runbook

Phase 2F is still test-only.

## Safe defaults

- localhost is allowed without arming
- external TEST endpoints require `--armed-test-network`
- starting a real external session requires `--armed-test-session`
- credentials must come from local env/file sources

## External test-session bring-up

Use a local untracked profile override and a local credential source. Example:

```bash
build/apps/moex_cert_runner \
  --profile /local/untracked/test_twime_tcp_external.local.yaml \
  --armed-test-network \
  --armed-test-session \
  --credentials-env MOEX_TWIME_TEST_CREDENTIALS \
  --scenario cert/scenarios/twime/live_test_session_establish.yaml \
  --output-dir build/twime-live-test
```

## Prohibited in Phase 2F

- production endpoints
- production profiles
- checked-in credentials
- unattended order flow
- live AlorEngine routing
