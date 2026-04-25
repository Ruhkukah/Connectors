# PLAZA II Phase 4F Connection Handshake

## Scope

Phase 4F validates the local CGate client to local P2MQRouter handshake for the existing TEST-only
`plaza2_repl` private-state runner. The phase keeps the Phase 3F/4E safety model intact:
explicit operator arming, no production connectivity, no order entry, no public market data, and no
checked-in credentials or endpoints.

## Non-Goals

- No `plaza2_trade`.
- No write-side C ABI.
- No order-entry samples or transactional trading.
- No firewall changes from repository scripts.
- No daemon, systemd, persistence, or public market-data profile work.

## Findings

The MOEX T1 router authenticated successfully before this phase. The remaining blocker was local
client bring-up:

- `cg_conn_open` returned `CG_ERR_UNSUPPORTED` when `cg_env_open` used the exchange account
  credential as `key=...`.
- The installed CGate samples distinguish the local CGate software key from the exchange account
  password. Using a CGate software key in `env_open_settings` gets `cg_conn_open` past
  `CG_ERR_UNSUPPORTED`.
- The router config inspected on the VPS did not expose a local `[AS:Local]` app/password map.
  `local_pass` was therefore not required for the tested profile.
- `p2tcp://127.0.0.1:4001` reached the local router and was used for the successful evidence run.
  `p2lrpcq` also moved past `cg_conn_open` when the software key was used, but p2tcp remained the
  evidence path for this phase.
- The installed runtime exposes dbscheme aliases for the private streams: `Trade`, `OrdBook`,
  `POS`, `PART`, and `REFDATA`.
- A private scoped scheme was used on the VPS so listener schemas include only Phase 3E projected
  private tables. Raw vendor scheme files and local profile files remain untracked.

## Code Change

Live TEST replay exposed a CGate callback payload mismatch for `CG_MSG_P2REPL_CLEARDELETED`.
The installed CGate header packs this payload with 4-byte alignment, so the wire/control payload is
16 bytes:

- `uint32_t table_idx`
- `int64_t table_rev`
- `uint32_t flags`

The prior mirror struct used native alignment and expected 24 bytes on x86_64, causing every live
`CLEAR_DELETED` callback to fail before it reached the projector. The adapter now decodes the packed
payload explicitly.

The live runner also now accepts `CLEAR_DELETED` inside a transaction and stages the affected
stream-owned clear in the private-state projector. That preserves commit-bounded visibility while
matching the live CGate lifecycle observed during snapshot replay.

If CGate reports a `CLEAR_DELETED` table index that is not covered by the reviewed runtime message
plan, the adapter still dispatches a stream-level clear event. Phase 3E currently clears private
state by stream for this control event, so treating an unknown clear-deleted table index as fatal is
unnecessarily strict for the private-state read-side profile.

## Operator Profile Guidance

Use an operator-local, untracked profile. Do not commit the profile, software key, account password,
router auth file, or evidence logs containing raw secrets.

The client environment settings should use a user-writable client CGate ini and a CGate software
key, not the exchange account password:

```yaml
runtime:
  env_open_settings: ini=/home/<user>/.config/moex-connector/cgate/client_t1.ini;key=<CGATE_SOFTWARE_KEY>
```

The exchange TEST account credential remains operator-local and must not be printed or committed.

Listener settings for the successful private-state run used the scoped scheme path and runtime
dbscheme aliases:

```yaml
listeners:
  FORTS_TRADE_REPL:
    settings: p2repl://FORTS_TRADE_REPL;scheme=|FILE|/home/<user>/.config/moex-connector/cgate/forts_private_required_scheme.ini|Trade
  FORTS_USERORDERBOOK_REPL:
    settings: p2repl://FORTS_USERORDERBOOK_REPL;scheme=|FILE|/home/<user>/.config/moex-connector/cgate/forts_private_required_scheme.ini|OrdBook
  FORTS_POS_REPL:
    settings: p2repl://FORTS_POS_REPL;scheme=|FILE|/home/<user>/.config/moex-connector/cgate/forts_private_required_scheme.ini|POS
  FORTS_PART_REPL:
    settings: p2repl://FORTS_PART_REPL;scheme=|FILE|/home/<user>/.config/moex-connector/cgate/forts_private_required_scheme.ini|PART
  FORTS_REFDATA_REPL:
    settings: p2repl://FORTS_REFDATA_REPL;scheme=|FILE|/home/<user>/.config/moex-connector/cgate/forts_private_required_scheme.ini|REFDATA
```

## Evidence

VPS evidence path:

```text
/home/<user>/moex/evidence/plaza2-test/20260425T093257Z-phase4f_packed_clear_deleted
```

Redacted result:

- `result=ok`
- `runner_state=Ready`
- `ready=true`
- `runtime_probe_ok=true`
- `scheme_drift_ok=true`
- `scheme_drift_status=CompatibleWithWarnings`
- `scheme_drift_fatal_count=0`
- required private listeners created and opened:
  `FORTS_TRADE_REPL`, `FORTS_USERORDERBOOK_REPL`, `FORTS_POS_REPL`, `FORTS_PART_REPL`,
  `FORTS_REFDATA_REPL`
- client log showed no `processClearDeleted` callback failures in the successful run
- private-state counts: sessions `0`, instruments `0`, matching map rows `0`, limits `1`,
  positions `0`, own orders `0`, own trades `0`

## Tests

Local macOS build:

```bash
cmake --build build -j4
ctest --test-dir build --output-on-failure -R \
  'plaza2_live_session_runner_test|plaza2_live_session_validation_test|plaza2_private_state_invalidation_test|plaza2_private_state_visibility_test|plaza2_private_state_projection_test|source_style_check'
```

Docker Linux build:

```bash
docker run --rm -v "$PWD:/work" -w /work "$MOEX_BUILD_IMAGE" bash -lc '
  set -euo pipefail
  cmake -S . -B build-docker-linux -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build build-docker-linux -j"$(nproc)" --target \
    moex_plaza2_test_runner plaza2_live_session_runner_test plaza2_private_state_invalidation_test
  ctest --test-dir build-docker-linux --output-on-failure -R "plaza2_live_session_runner_test|plaza2_private_state_invalidation_test|source_style_check"
'
```

## Known Limitations

- The successful run used an operator-local scoped scheme file that is not committed.
- The tracked example profiles still require operator adaptation for the installed runtime aliases
  and local CGate software key.
- Private-state counts depend on current TEST account activity and may be zero for most domains.
- Router exposure remains an operator concern. P2MQRouter was observed listening on all interfaces;
  verify provider or host firewall policy restricts access to trusted hosts only.

## Next Recommended Phase

Convert the operator-local Phase 4F profile discoveries into safe reusable profile generation:
software-key handling, scoped private scheme materialization, runtime alias selection, and clearer
evidence output for per-stream online/snapshot-complete state.
