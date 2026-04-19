# TWIME Phase 2E Test Endpoint Gating

## Scope

Phase 2E extends the Phase 2D TCP transport so it can support explicitly armed
external **test** endpoints. The default runtime path remains localhost-only.

This phase adds:

- explicit test-network gate validation in front of `TwimeTcpTransport::open()`
- external test-endpoint profile templates that stay non-usable by default
- local-only credential providers for env/file loading
- credential redaction for runner logs and summaries
- cert-runner plumbing for test-gated validation and optional test-only session
  attempts

## What Changed From Phase 2D

Phase 2D accepted only exact loopback hosts in the transport itself. Phase 2E
keeps that safe default, but allows a second path for non-loopback targets when
all of the following are true:

1. `environment == test`
2. `twime_tcp.test_network_gate.external_test_endpoint_enabled == true`
3. runtime is armed with `--armed-test-network` or `MOEX_ARM_TEST_NETWORK=1`
4. credentials are loaded from a local env/file source
5. endpoint validation passes

Without those conditions, non-loopback targets are rejected before socket
creation.

## Localhost Default Still Applies

The default checked-in TCP profiles remain safe:

- `profiles/test_twime_tcp_loopback.yaml`
- `profiles/test_twime_tcp_fragmentation.yaml`
- `profiles/test_twime_tcp_remote_close.yaml`
- `profiles/test_twime_tcp_external_local.example.yaml`

All of them stay on `127.0.0.1`, `::1`, or `localhost`.

## External Test Endpoint Gating

Tracked profiles may include only:

- loopback hosts; or
- a placeholder host such as `TEST_ENDPOINT_HOST_PLACEHOLDER`

Tracked profiles must **not** include real external hosts. A real test endpoint
must come from an untracked local override or CLI/runtime override.

The runtime gate blocks non-loopback access unless test-network arming is
present. It also blocks production-like MOEX or broker hostnames in this phase.

## Credentials

Phase 2E adds local-only credential plumbing:

- `EnvTwimeCredentialProvider`
- `FileTwimeCredentialProvider`

Credentials are never checked into the repo. Tracked templates may only point
to placeholders such as:

- `source: env`
- `env_var: MOEX_TWIME_TEST_CREDENTIALS`

External test-endpoint mode fails before network open when credentials are not
available from the configured local source.

## Cert Runner

`moex_cert_runner` and `moex_twime_cert_runner` now accept test-network flags
for profile-driven validation:

```text
--profile <path>
--armed-test-network
--validate-only
--credentials-env MOEX_TWIME_TEST_CREDENTIALS
--credentials-file /local/path
```

Safe defaults remain in force:

- without `--armed-test-network`, non-loopback profile runs fail
- without credentials, external test-network profile runs fail before connect
- CI uses validation-only checks for external fixtures and never opens a real
  external socket

## Explicit Non-Goals

Phase 2E still does **not** add:

- production endpoints
- broker production endpoints
- checked-in credentials
- default non-loopback connectivity
- default live order submission
- default C ABI live order routing
- default AlorEngine live routing

## Operator Guidance

For local test-environment work:

1. start from a checked-in template
2. create an untracked local override or pass runtime overrides
3. load credentials from local env/file sources
4. arm test-network access explicitly

If those steps are not taken, the transport remains localhost-only.
