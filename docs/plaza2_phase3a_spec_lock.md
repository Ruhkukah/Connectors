# Phase 3A: PLAZA II Repl Spec Lock

Phase 3A locks the PLAZA II replication surface into deterministic repo
manifests and machine-readable inventories. The output is intentionally narrow:
it records the documentation roots, the approved replication stream/table
surface, the deferred runtime-local `.ini` expectations, and the profile
matrix needed for the next implementation phases.

## Scope

- deterministic `spec-lock/prod|test/plaza2/manifest.yaml`
- machine-readable `plaza2_streams`, `plaza2_tables`, and `plaza2_messages`
  inventories
- profile-to-stream mapping for:
  - `plaza2_private_offline`
  - `plaza2_private_test`
  - `plaza2_private_prod`
  - `plaza2_private_plus_aggr20`
  - `plaza2_private_plus_ordlog`
- locked documentation artifact hashes from the existing committed JSON
  manifests
- explicit runtime-local expectations for `forts_scheme.ini` and the router
  `.ini` files needed later for prod/test bring-up

## Non-goals

- no CGate runtime adapter
- no listener/connection lifecycle code
- no state projection engine
- no live session bring-up
- no C ABI changes
- no `plaza2_trade` work

## Architecture

Phase 3A adds one deterministic materializer:

- `tools/plaza2_phase3a_materialize.py`

The tool owns the checked-in YAML outputs and validates them in `--check`
mode. It uses:

- committed PLAZA II doc manifests under `spec-lock/prod/plaza2/*/manifest.json`
  as the locked source for artifact hashes and upstream filenames
- curated, reviewable stream/table/message metadata derived from the approved
  replication track

This keeps CI offline-safe. No standard test needs the vendor cache payloads
from `spec-lock/**/cache/`.

## State Model

Phase 3A does not implement runtime state. It locks the state-bearing surfaces
needed for later phases:

- private-state core streams:
  - refdata, info, limits, positions, own orders/trades, user order book,
    session state, instrument state, security-group state
- optional public streams:
  - lightweight aggregated book family with `FORTS_AGGR20_REPL` as the default
    target
  - heavy ordlog/order-book family with matching-partition awareness
- replication/runtime signals:
  - `CG_MSG_P2REPL_CLEARDELETED`
  - `CG_MSG_P2REPL_LIFENUM`
  - `ONLINE`
  - `mode=snapshot+online`
  - resume `lifenum` / `rev.<table>` parameters
  - `_MATCH${id}` stream partition suffix

## Safety Model

- no credentials are added
- no production endpoints are added
- no live behavior is enabled
- vendor cache payloads remain gitignored
- local CGate runtime files remain uncommitted
- only hashes, filenames, and derived inventory are tracked in git

## Performance Considerations

Phase 3A is metadata only. It preserves the 2-4 vCPU deployment target by
keeping the planned stream surface explicit:

- private-state streams stay separate from heavy ordlog streams
- aggregated public data stays an opt-in profile
- matching-partitioned streams are marked now so Phase 3H does not need a
  redesign later

## Tests Added

- `plaza2_phase3a_check`
  - runs `build/tools/plaza2_phase3a_materialize --check`
  - validates deterministic checked-in output
  - validates required stream/table/message coverage
  - validates profile references and runtime expectation structure

The existing `matrix_validate_repo` test continues to validate referential
integrity across the inventory files.

## Known Limitations

- local CGate scheme/runtime `.ini` files are not yet hash-locked in-repo
- the public doc cache payloads themselves are still external/gitignored
- the inventory is a spec lock, not a parser or code generator
- callback-level vendor message constants beyond the approved repl/runtime
  surface remain deferred to later phases

## What Is Locked

- committed doc-manifest rows for `plaza_docs` and `cgate_docs`
- upstream filenames, canonical URLs, sizes, and sha256 hashes from those rows
- the approved PLAZA II replication streams and tables needed for:
  - private-state replication
  - aggregated public market data
  - full ordlog/order-book work
- the runtime-local expectation that `forts_scheme.ini` is the local scheme file
  to lock later under `spec-lock/prod|test/plaza2/scheme/manifest.json`

## Intentionally Not Committed

- vendor cache payloads under `spec-lock/**/cache/`
- installed CGate runtime trees
- `forts_scheme.ini`
- `prod.ini`, `t0.ini`, `t1.ini`, `game.ini`
- `links_public.*.ini`
- `auth_client.ini`, `client_router.ini`, `router.ini`
- any credentials or endpoint secrets

The planned test-gated bring-up flags locked by this phase are:

- `--armed-test-network`
- `--armed-test-session`
- `--armed-test-plaza2`

## Next Recommended Phase

Proceed to Phase 3B: deterministic PLAZA II scheme metadata/code generation,
using the locked stream/table surface from this phase as the initial required
coverage set.
