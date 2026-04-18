# ABI Policy

## Versioning

- `MOEX_C_ABI_VERSION` is append-only.
- Breaking ABI changes require a version bump.
- Non-breaking additions may keep the same ABI version if they only append new enums, structs, or functions.

## `struct_size`

- Every input/output struct exposed across the C ABI must start with `struct_size`.
- Callers set `struct_size = sizeof(struct)` for the version they were compiled against.
- Callees may accept larger `struct_size` values and must only read the known prefix.
- Output structs returned by native code must set `struct_size` to the native layout size.

## Enum Extension

- Enums are append-only.
- Existing numeric values must never change.
- Unknown enum values must be handled defensively by managed callers.

## String Ownership

- `const char*` inputs are caller-owned and must remain valid for the duration of the call only.
- The native layer does not retain caller string pointers after the call returns.
- The Phase 0 ABI does not expose native-owned heap strings to managed callers.

## Callback Threading

- Low-rate callbacks are reserved for connector-owned callback threads.
- Callbacks must be non-blocking and must not call back into connector destruction.
- Phase 0 stubs do not dispatch real callbacks, but future implementations will preserve this rule.

## Polling and Backpressure

- High-rate event flow uses polling, not one managed callback per event.
- Private trades, private order status, and full order log are lossless channels and must never silently drop.
- Public L1 and diagnostics may use an explicit `drop_oldest` policy.
- Counters for `produced`, `polled`, `dropped`, `high_watermark`, and overflow state are part of the ABI contract.

## Exceptions

- No C++ exceptions may cross the ABI boundary.
- Exported C ABI functions must catch internal exceptions and convert them into stable error codes.
