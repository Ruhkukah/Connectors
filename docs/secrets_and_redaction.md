# Secrets and Redaction

## Public Repo Rule

This repository is public. Do not commit:

- TWIME credentials
- broker configs
- production or broker endpoints
- production logs
- certification logs captured from real environments
- latency or topology data from live infrastructure

## Local-Only Credential Sources

Phase 2E allows TWIME test credentials only from local sources:

- environment variables, for example `MOEX_TWIME_TEST_CREDENTIALS`
- local files outside tracked repo configuration

Checked-in profiles may reference those sources, but they must not contain the
credential values themselves.

## Redaction Rules

When credentials or account-like identifiers appear in diagnostics:

- credentials are fully redacted as `[REDACTED]`
- account-like values are partially masked when useful for diagnostics
- external test-network logs should be labeled with
  `[TEST-NETWORK-ARMED]`

This applies to:

- cert-runner logs
- transport diagnostics
- profile summaries
- debug traces produced by test-endpoint validation

## Local Files and Ignore Rules

The repo ignores local secret material through patterns such as:

- `profiles/*.local.yaml`
- `secrets/`
- `*.secrets.yaml`

Use those paths for local-only overrides when you need to test a non-loopback
TWIME test endpoint.
