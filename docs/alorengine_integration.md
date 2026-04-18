# AlorEngine Integration Boundary

Phase 0 targets the current AlorEngine seams instead of introducing a new monolithic connector abstraction.

Mapped seams:

- trading request path through `ITradingClientRouter` / `LiveTradingClient`
- `IOrderBookService`
- `IMarketDataPublisherService`
- `IInstrumentCatalog`
- `IOrderStatusBus`
- `ITradeExecutionBus`
- `IPositionBus`
- DTC-facing event production

The managed/native boundary remains:

`AlorEngine -> managed wrapper -> C ABI -> native suite`

Phase 1 adds a stub-only shadow replay path:

- `MoexNativeLibrary` dynamically loads the native C ABI and validates struct sizes/versioning.
- `MoexNativeConnector` exposes SafeHandle-backed lifecycle, profile load, replay load, batch polling, health, backpressure, and low-rate callbacks.
- The optional `ShadowReplay` harness compiles against a local `AlorEngine.csproj` when `AlorEngineProject` is supplied and projects synthetic native events into:
  - `ITradingClientRouter` via a pass-through shadow router
  - `IOrderBookService`
  - `IMarketDataPublisherService`
  - `IInstrumentCatalog`
  - `IOrderStatusBus`
  - `ITradeExecutionBus`
  - `IPositionBus`

The native path remains read-only in shadow mode. ALOR stays primary for order routing.
