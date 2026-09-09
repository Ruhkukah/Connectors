# Public 9.9 wire qualification

Phase B begins at `669a16d`; no material field difference was found in the ten declared public tables.
The lock is `spec-lock/test/plaza2/public99/wire.json`; generated native records live in
`protocols/plaza2_cgate/generated/plaza2_public_wire.hpp`.

## Access diagnosis

The FTP server presents a certificate issued by Russian Trusted Sub CA. Its root was absent from the local trust store.
MOEX's [certificate migration notice](https://www.moex.com/n103531?nt=209) directs users to the official certificates.
The root was downloaded over normally verified HTTPS from `https://gu-st.ru/content/lending/russian_trusted_root_ca_pem.crt`.
A request-local `--cacert` / Python SSLContext then verified MOEX's chain, hostname and validity.
No `-k`, global trust-store change, router login or exchange connection was used.

## Evidence

Exact official TEST INIs and the downloaded SDK contain matching public definitions. Header, library and SDK archive hashes
match the prior 9.9 lock. The current full scheme hash differs from the old deployment snapshot; do not replace that deployment
fingerprint implicitly. Public logical field names/order/types have no differences.

The SDK `schemetool makesrc` generated both public C headers. An x86-64 compiler in a `--network none` container measured every
`sizeof` and `offsetof`; the frozen lock records those values. Code generation uses the existing `plaza2_codegen.py` entry point
and checks the entire reviewed field sequence before producing native structs and layout assertions. No production struct
is hand-maintained in parallel with generated bindings.

`cg_time_t` is 10 bytes with milliseconds at offset 8. Records use packing 4. Decimal `d16.5` is 11 bytes; prices remain exact
BCD with an optional checked integer mantissa conversion at scale 5. `moment_ns` is unsigned absolute UTC epoch nanoseconds.
The schemas declare no optional fields. Callback null maps are nevertheless retained explicitly; null service keys cannot
be accepted as valid replication identities. Missing fields are never silently replaced with economic zero.

`tests/plaza2_cgate/plaza2_public_decimal_vendor_check.cpp` compared **100,004** deterministic positive/negative/zero/extreme
BCD values with the official `cg_bcd_get`: zero failures, no network. Raw bytes remain retained even when a convenience value
is available. Unknown action and xstatus/xstatus2 bits are preserved. No ORDLOG encoder is added to the connector.

## Reproduction

Download and hash the source files listed in the lock. Keep vendor material in the ignored `public99/cache` directory or a
separate workspace. With SDK binaries on an isolated Linux x86-64 host/container:

```sh
schemetool makesrc -o ordlog-official.h ordLog_trades.ini CustReplScheme
schemetool makesrc -o ordbook-official.h ordbook.ini CustReplScheme
```

`tools/plaza2_public_wire_capture.py --directory <artifact-directory>` prepares `layout-probe.cpp` from every INI field.
Compile it with the SDK include directory; its CSV reports all native offsets and sizes. Compare these to the lock.
The raw headers are evidence inputs, not a second committed binding. Regenerate the connector with:

```sh
python3 tools/plaza2_codegen.py \
  --schema protocols/plaza2_cgate/schema/plaza2_forts_reviewed.ini \
  --out protocols/plaza2_cgate/generated \
  --public-wire-lock spec-lock/test/plaza2/public99/wire.json
```

Add `--check` for reproducibility verification. Compile the decimal vendor check with the same SDK library plus repository
include/generated paths, C++20, and run with networking disabled. It opens only the local CGate environment; no connection,
listener, publisher, router or credentials are created.

The `plaza2_public_wire_test` and `plaza2_public_wire_codegen_check` CTest entries cover the local native decode/layout path.
See [recovery contract](P2ORDBOOK_ORDLOG_RECOVERY_9_9.md) for composite listener constraints. C1 may proceed independently;
no current L3 state or certification performance is claimed.
