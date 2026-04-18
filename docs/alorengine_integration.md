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
