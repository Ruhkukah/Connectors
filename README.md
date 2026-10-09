# MoexConnector 1.0.0

MoexConnector is a C++20 connector for own-account trading through MOEX PLAZA II CGate. The declared commands are AddOrder, DelOrder, DelUserOrders and MoveOrder. Certification instructions, questionnaire answers and the operator manual are in [CERTIFICATION.md](CERTIFICATION.md).

The runtime target is the official CGate 6.102.0 distributive shipped with Spectra 9.9 on Linux. Supply the vendor runtime, configuration and negotiated schemes locally. Credentials remain outside the repository. The default tests load the fake CGate runtime and use no exchange connection.

The first certificate covers private replication, reference/session state, AGGR20 market data and the declared command path. Public FORTS_DEALS_REPL remains opt-in and disabled by default. FullOrderLog, TWIME, C ABI V1–V3, .NET integration and DTC order entry are outside this scope. The removed implementations are preserved at `archive/pre-cgate-certification-remediation-20260930`.

The read-only DTC server publishes market data for consumers such as Kairos. Trading uses the native CGate command path and configured account/instrument limits.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`BUILD_TESTING=OFF` omits the fake runtime and test targets. `MOEX_BUILD_APPS=OFF` builds the libraries without operator applications. The `moexctl` startup record identifies product version 1.0.0, `source_git_sha` and the executable SHA-256; the DTC receipt also includes the source revision when the checkout provides one.

Useful focused checks:

```sh
ctest --test-dir build -L plaza2 --output-on-failure
ctest --test-dir build -L dtc --output-on-failure
ctest --test-dir build -L sanitizer --output-on-failure
```

Use `moex_connector_host_dtc_runner --help` and `moexctl --help` for operator arguments. Vendor installation helpers remain under `scripts/vps/`.

License: MIT. See [LICENSE](LICENSE). See [SECURITY.md](SECURITY.md) for the credential and private-data policy.
