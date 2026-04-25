# Phase 5E PLAZA II TEST Order-Entry Bring-Up

## Scope

Phase 5E adds the first tightly gated PLAZA II TEST order-entry path for the near-term
`plaza2_repl + plaza2_trade` profile. The implemented live subset is intentionally limited
to one tiny `AddOrder` followed by `DelOrder` for the same confirmed order.

The runner always defaults to dry-run. A live TEST command requires all explicit arm flags
and `--send-test-order`.

## Non-Goals

This phase does not add production connectivity, strategy logic, write-side C ABI exports,
AlorEngine integration, public market-data expansion, persistence, hidden reconnect loops,
or live support for move, iceberg, mass-cancel, delete-by-mask, or COD heartbeat commands.

## Safety Model

The required arm flags are:

- `--armed-test-network`
- `--armed-test-session`
- `--armed-test-plaza2`
- `--armed-test-order-entry`
- `--armed-test-tiny-order`

Live TEST send additionally requires `--send-test-order`. Without it, the wrapper appends
`--dry-run`, validates the profile, encodes the `AddOrder`, writes evidence, and stops before
publisher submission.

Tracked profiles contain only placeholders or synthetic values. Local TEST credentials,
software keys, auth files, endpoint overlays, and real account fields remain outside Git.

## Profile Model

The tracked templates are:

- `profiles/test_plaza2_trade_order_entry.template.yaml`
- `profiles/test_plaza2_trade_order_entry_local.example.yaml`

The local VPS profile should be copied to:

`~/.config/moex-connector/profiles/plaza2_trade_test_order.local.yaml`

The profile must provide explicit operator-owned values for `isin_id`, `broker_code`,
`client_code`, `side`, `price`, `quantity`, `max_quantity`, `ext_id`, and
`client_transaction_id_prefix`. The runner does not auto-pick instruments or prices from
AGGR20.

## Execution Flow

The native runner validates the TEST-only profile, arms, tiny-order limits, runtime settings,
and live command whitelist. Dry-run encodes `AddOrder` through the Phase 5B codec and stops.

In live TEST mode the runner starts the existing private-state read-side runner, waits for
private streams to become ready, opens a CGate publisher on the same live connection, posts
one `AddOrder`, waits for private-state confirmation, builds `DelOrder` from the confirmed
order id, posts it, waits for cancellation confirmation, and stops.

## Confirmation Model

Evidence distinguishes:

- `command_encoded`
- `command_submitted`
- `reply_received`
- `reply_accepted`
- `private_order_seen`
- `user_orderbook_seen`
- `cancel_submitted`
- `cancel_confirmed`

Phase 5E currently treats private replication confirmation as mandatory for success. If a
reply is absent but replication confirms, the evidence keeps reply fields false instead of
pretending a reply was observed.

## Evidence Artifacts

The runner writes:

- `startup.json`
- `runtime_probe.json`
- `scheme_drift.json`
- `order_entry_plan.json`
- `pre_send_summary.json`
- `add_order_reply.json`
- `add_order_confirmation.json`
- `cancel_order_reply.json`
- `cancel_order_confirmation.json`
- `final_health.json`
- `operator.log`
- `run_manifest.json`

Account and client fields are redacted in evidence summaries. Credentials, software keys,
auth file contents, and raw secret values are never written.

## Failure Classifications

The runner reports explicit classifications including `missing_arm_flag`,
`production_profile_rejected`, `invalid_order_profile`, `runtime_probe_failed`,
`scheme_drift_incompatible`, `private_state_not_ready`, `aggr20_not_ready`,
`command_validation_failed`, `publisher_open_failed`, `command_send_failed`,
`reply_rejected`, `reply_timeout`, `replication_confirmation_timeout`,
`cancel_send_failed`, `cancel_reply_rejected`, `cancel_confirmation_timeout`, and `unknown`.

## VPS Runbook

Dry-run first:

```bash
source ~/.config/moex-connector/secrets/plaza2_test.env
run_id="$(date -u +%Y%m%dT%H%M%SZ)-trade-dry-run"
mkdir -p "$HOME/moex/evidence/plaza2-trade/$run_id"

~/moex/connector/scripts/vps/plaza2_trade_test_order_evidence.sh \
  --bundle-root ~/moex/connector \
  --profile ~/.config/moex-connector/profiles/plaza2_trade_test_order.local.yaml \
  --secret-env-file ~/.config/moex-connector/secrets/plaza2_test.env \
  --output-dir "$HOME/moex/evidence/plaza2-trade/$run_id" \
  --armed-test-network \
  --armed-test-session \
  --armed-test-plaza2 \
  --armed-test-order-entry \
  --armed-test-tiny-order \
  --dry-run
```

After operator review of `pre_send_summary.json`, run live TEST:

```bash
source ~/.config/moex-connector/secrets/plaza2_test.env
run_id="$(date -u +%Y%m%dT%H%M%SZ)-trade-live-test"
mkdir -p "$HOME/moex/evidence/plaza2-trade/$run_id"

~/moex/connector/scripts/vps/plaza2_trade_test_order_evidence.sh \
  --bundle-root ~/moex/connector \
  --profile ~/.config/moex-connector/profiles/plaza2_trade_test_order.local.yaml \
  --secret-env-file ~/.config/moex-connector/secrets/plaza2_test.env \
  --output-dir "$HOME/moex/evidence/plaza2-trade/$run_id" \
  --armed-test-network \
  --armed-test-session \
  --armed-test-plaza2 \
  --armed-test-order-entry \
  --armed-test-tiny-order \
  --send-test-order \
  --max-polls 512
```

## Tests Added

CI covers arm-gating refusal, live-send refusal without `--send-test-order`, invalid tiny
order profiles, dry-run encoding without a live gateway, and fake publisher/private-state
accept/cancel confirmation.

## Known Limitations

Reply listener decoding is not yet promoted to a success requirement; Phase 5E records reply
fields explicitly and requires private replication confirmation. Only `AddOrder` and
`DelOrder` are live-enabled. All other Phase 5B commands remain offline-only.

## Next Phase

Phase 5F should be PLAZA II order-entry confirmation hardening and safety pack: rejection
taxonomy, cancellation guarantees, stuck-order safety, confirmation timeouts, replayable
evidence packs, and certification-style order scenarios. Production order-entry remains
deferred.
