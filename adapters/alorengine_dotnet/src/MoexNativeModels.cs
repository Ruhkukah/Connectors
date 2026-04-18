using System.Text;

namespace MoexConnector.AlorEngine;

public enum MoexResult
{
    Ok = 0,
    InvalidArgument = 1,
    NotSupported = 2,
    NotInitialized = 3,
    AlreadyStarted = 4,
    NotStarted = 5,
    Overflow = 6,
    InternalError = 255
}

public enum MoexEventType
{
    Unspecified = 0,
    ConnectorStatus = 1,
    OrderStatus = 2,
    PrivateTrade = 3,
    Position = 4,
    PublicL1 = 5,
    PublicDiagnostic = 6,
    FullOrderLog = 7,
    CertStep = 8,
    OrderBook = 9,
    Instrument = 10,
    ReplayState = 11,
    PublicTrade = 12
}

public enum MoexReplayState
{
    Unspecified = 0,
    Started = 1,
    Drained = 2
}

public enum MoexNativeOrderStatus
{
    Unspecified = 0,
    New = 1,
    PartialFill = 2,
    Filled = 3,
    Canceled = 4,
    Rejected = 5
}

public enum MoexSourceConnector
{
    Unknown = 0,
    TwimeTrade = 1,
    FixTrade = 2,
    FastMd = 3,
    SimbaMd = 4,
    Plaza2Repl = 5,
    Plaza2Trade = 6
}

public readonly record struct MoexConnectorEventHeader(
    uint StructSize,
    ushort AbiVersion,
    MoexEventType EventType,
    ulong ConnectorSequence,
    ulong SourceSequence,
    long MonotonicTimeNs,
    long ExchangeTimeUtcNs,
    long SourceTimeUtcNs,
    long SocketReceiveMonotonicNs,
    long DecodeMonotonicNs,
    long PublishMonotonicNs,
    long ManagedPollMonotonicNs,
    MoexSourceConnector SourceConnector,
    uint Flags);

public readonly record struct MoexBackpressureSnapshot(
    ulong Produced,
    ulong Polled,
    ulong Dropped,
    ulong HighWatermark,
    bool Overflowed);

public readonly record struct MoexHealthView(
    uint ConnectorState,
    uint ActiveProfileKind,
    bool ProdArmed,
    bool ShadowModeEnabled);

public sealed record MoexConnectorEvent(
    MoexConnectorEventHeader Header,
    uint PayloadSize,
    ushort PayloadVersion,
    MoexReplayState ReplayState,
    MoexNativeOrderStatus Status,
    int Side,
    int Level,
    int UpdateType,
    double Price,
    double Quantity,
    double SecondaryPrice,
    double SecondaryQuantity,
    double CumulativeQuantity,
    double RemainingQuantity,
    double AveragePrice,
    double OpenProfitLoss,
    bool PreferOrderBookL1,
    bool Existing,
    bool ReadOnlyShadow,
    string Symbol,
    string Board,
    string Exchange,
    string InstrumentGroup,
    string Portfolio,
    string TradeAccount,
    string ServerOrderId,
    string ClientOrderId,
    string InfoText);

public sealed class MoexNativeException : InvalidOperationException
{
    public MoexNativeException(string operation, MoexResult result)
        : base($"{operation} failed with {result}.")
    {
        Operation = operation;
        Result = result;
    }

    public string Operation { get; }

    public MoexResult Result { get; }
}

internal static class MoexNativeMapper
{
    public static MoexConnectorEvent Map(MoexNativeInterop.NativeMoexPolledEvent native) =>
        new(
            Map(native.header),
            native.payload_size,
            native.payload_version,
            (MoexReplayState)native.replay_state,
            (MoexNativeOrderStatus)native.status,
            native.side,
            native.level,
            native.update_type,
            native.price,
            native.quantity,
            native.secondary_price,
            native.secondary_quantity,
            native.cumulative_quantity,
            native.remaining_quantity,
            native.average_price,
            native.open_profit_loss,
            native.prefer_order_book_l1 != 0,
            native.existing != 0,
            native.read_only_shadow != 0,
            DecodeUtf8(native.symbol),
            DecodeUtf8(native.board),
            DecodeUtf8(native.exchange),
            DecodeUtf8(native.instrument_group),
            DecodeUtf8(native.portfolio),
            DecodeUtf8(native.trade_account),
            DecodeUtf8(native.server_order_id),
            DecodeUtf8(native.client_order_id),
            DecodeUtf8(native.info_text));

    public static MoexConnectorEventHeader Map(MoexNativeInterop.NativeMoexEventHeader native) =>
        new(
            native.struct_size,
            native.abi_version,
            (MoexEventType)native.event_type,
            native.connector_seq,
            native.source_seq,
            native.monotonic_time_ns,
            native.exchange_time_utc_ns,
            native.source_time_utc_ns,
            native.socket_receive_monotonic_ns,
            native.decode_monotonic_ns,
            native.publish_monotonic_ns,
            native.managed_poll_monotonic_ns,
            (MoexSourceConnector)native.source_connector,
            native.flags);

    public static MoexHealthView Map(MoexNativeInterop.NativeMoexHealthSnapshot native) =>
        new(native.connector_state, native.active_profile_kind, native.prod_armed != 0, native.shadow_mode_enabled != 0);

    public static MoexBackpressureSnapshot Map(MoexNativeInterop.NativeMoexBackpressureCounters native) =>
        new(native.produced, native.polled, native.dropped, native.high_watermark, native.overflowed != 0);

    private static string DecodeUtf8(byte[]? bytes)
    {
        if (bytes is null || bytes.Length == 0)
        {
            return string.Empty;
        }

        var terminator = Array.IndexOf(bytes, (byte)0);
        var count = terminator >= 0 ? terminator : bytes.Length;
        return count == 0 ? string.Empty : Encoding.UTF8.GetString(bytes, 0, count);
    }
}
