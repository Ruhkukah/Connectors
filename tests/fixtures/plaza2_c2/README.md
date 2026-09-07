# C2 fixture provenance

`sdk_probe.cpp` is the reproducible no-network SDK experiment. It attempts only container loopback port 1 and calls no publisher APIs. Copy it into
the Phase B evidence directory containing the locked SDK and INIs, then run:

```sh
docker run --rm --network none --platform linux/amd64 \
  -v /tmp/plaza2-offline:/evidence -w /evidence moex-connectors-phase0:local \
  bash -c 'g++ -std=c++20 c2-sdk-probe.cpp -I sdk/cgate/include -L sdk/cgate/lib -lcgate -o c2-sdk-probe && \
  LD_LIBRARY_PATH=/evidence/sdk/cgate/lib ./c2-sdk-probe'
```

The image must have a compiler and the public SDK dependencies. It is not committed or fetched by this test. The source is
built against the actual vendor header, not the fake ABI. Output is locked in
`docs/review/plaza2_c2_preflight_20260907/sdk_probe.log`.

**No actual negotiated composite fixture exists yet: REQUIRES_T1_CAPTURE for both pairs.** The fake shared library models
regular `[orders_log, info, orders]` and multileg `[multileg_orders_log, info, multileg_orders]`, plus reversed permutations.
These layouts reuse qualified source fields and arbitrary composite indices. They establish test coverage only; do not
rename them as a MOEX capture. The fake library and callback tests are the single executable fixture source:

- `tests/plaza2_cgate/fake_cgate_runtime.cpp`: synthetic descriptor construction and callback/error injection.
- `tests/plaza2_cgate/plaza2_c2_preflight_test.cpp`: schema binding and ordered regular/multileg callback scenarios.
- `tests/plaza2_cgate/plaza2_c2_reference.hpp`: pure conditional mutation/recovery reference.

Replay scope and unresolved identity/boundary questions are in `docs/plaza2/PUBLIC_L3_MUTATION_CONTRACT_9_9.md`.
