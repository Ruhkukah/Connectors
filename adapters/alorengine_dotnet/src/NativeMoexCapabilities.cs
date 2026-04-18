namespace MoexConnector.AlorEngine;

[System.Flags]
public enum NativeMoexCapabilities
{
    None = 0,
    Orders = 1 << 0,
    MarketDataL1 = 1 << 1,
    MarketDepthAggregated = 1 << 2,
    MarketDepthFullOrderLog = 1 << 3,
    Trades = 1 << 4,
    Positions = 1 << 5,
    Limits = 1 << 6,
    Options = 1 << 7,
    Futures = 1 << 8,
    MassCancel = 1 << 9,
    CancelOnDisconnect = 1 << 10
}
