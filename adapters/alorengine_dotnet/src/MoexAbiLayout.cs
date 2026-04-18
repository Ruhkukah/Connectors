using System.Runtime.InteropServices;

namespace MoexConnector.AlorEngine;

public sealed record MoexAbiStructLayout(
    string Name,
    uint NativeSize,
    int ManagedSize,
    uint NativeAlignment,
    int ManagedAlignment);

public sealed class MoexAbiLayout
{
    private readonly IReadOnlyList<MoexAbiStructLayout> _structs;

    internal MoexAbiLayout(IReadOnlyList<MoexAbiStructLayout> structs)
    {
        _structs = structs;
    }

    public IReadOnlyList<MoexAbiStructLayout> Structs => _structs;

    internal static int AlignmentOf<TProbe>(string fieldName) where TProbe : struct =>
        checked((int)Marshal.OffsetOf<TProbe>(fieldName));

    [StructLayout(LayoutKind.Sequential)]
    internal struct EventHeaderProbe
    {
        public byte prefix;
        public MoexNativeInterop.NativeMoexEventHeader value;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct BackpressureCountersProbe
    {
        public byte prefix;
        public MoexNativeInterop.NativeMoexBackpressureCounters value;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct HealthSnapshotProbe
    {
        public byte prefix;
        public MoexNativeInterop.NativeMoexHealthSnapshot value;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ConnectorCreateParamsProbe
    {
        public byte prefix;
        public MoexNativeInterop.NativeMoexConnectorCreateParams value;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ProfileLoadParamsProbe
    {
        public byte prefix;
        public MoexNativeInterop.NativeMoexProfileLoadParams value;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct OrderSubmitRequestProbe
    {
        public byte prefix;
        public MoexNativeInterop.NativeMoexOrderSubmitRequest value;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct OrderCancelRequestProbe
    {
        public byte prefix;
        public MoexNativeInterop.NativeMoexOrderCancelRequest value;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct OrderReplaceRequestProbe
    {
        public byte prefix;
        public MoexNativeInterop.NativeMoexOrderReplaceRequest value;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct MassCancelRequestProbe
    {
        public byte prefix;
        public MoexNativeInterop.NativeMoexMassCancelRequest value;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct SubscriptionRequestProbe
    {
        public byte prefix;
        public MoexNativeInterop.NativeMoexSubscriptionRequest value;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PolledEventProbe
    {
        public byte prefix;
        public MoexNativeInterop.NativeMoexPolledEvent value;
    }
}
