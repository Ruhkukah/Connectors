using System.Runtime.InteropServices;

namespace MoexConnector.AlorEngine;

internal static class MoexNativeInterop
{
    internal const ushort AbiVersion = 1;
    internal const int SymbolCapacity = 32;
    internal const int BoardCapacity = 16;
    internal const int ExchangeCapacity = 16;
    internal const int GroupCapacity = 32;
    internal const int PortfolioCapacity = 32;
    internal const int AccountCapacity = 32;
    internal const int OrderIdCapacity = 40;
    internal const int InfoCapacity = 128;

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeMoexEventHeader
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort event_type;
        public ulong connector_seq;
        public ulong source_seq;
        public long monotonic_time_ns;
        public long exchange_time_utc_ns;
        public long source_time_utc_ns;
        public long socket_receive_monotonic_ns;
        public long decode_monotonic_ns;
        public long publish_monotonic_ns;
        public long managed_poll_monotonic_ns;
        public uint source_connector;
        public uint flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeMoexBackpressureCounters
    {
        public ulong produced;
        public ulong polled;
        public ulong dropped;
        public ulong high_watermark;
        public byte overflowed;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 7)]
        public byte[]? reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeMoexHealthSnapshot
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort reserved0;
        public uint connector_state;
        public uint active_profile_kind;
        public byte prod_armed;
        public byte shadow_mode_enabled;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 6)]
        public byte[]? reserved1;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeMoexConnectorCreateParams
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort reserved0;
        public IntPtr connector_name;
        public IntPtr instance_id;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeMoexProfileLoadParams
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort reserved0;
        public IntPtr profile_path;
        public byte armed;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 7)]
        public byte[]? reserved1;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeMoexOrderSubmitRequest
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort reserved0;
        public IntPtr profile_id;
        public IntPtr symbol;
        public IntPtr account;
        public IntPtr client_order_id;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeMoexOrderCancelRequest
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort reserved0;
        public IntPtr profile_id;
        public IntPtr account;
        public IntPtr server_order_id;
        public IntPtr client_order_id;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeMoexOrderReplaceRequest
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort reserved0;
        public IntPtr profile_id;
        public IntPtr account;
        public IntPtr server_order_id;
        public IntPtr client_order_id;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeMoexMassCancelRequest
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort reserved0;
        public IntPtr profile_id;
        public IntPtr account;
        public IntPtr instrument_scope;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeMoexSubscriptionRequest
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort reserved0;
        public IntPtr profile_id;
        public IntPtr stream_name;
        public IntPtr symbol;
        public IntPtr board;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeMoexPolledEvent
    {
        public NativeMoexEventHeader header;
        public uint payload_size;
        public ushort payload_version;
        public ushort replay_state;
        public int status;
        public int side;
        public int level;
        public int update_type;
        public double price;
        public double quantity;
        public double secondary_price;
        public double secondary_quantity;
        public double cumulative_quantity;
        public double remaining_quantity;
        public double average_price;
        public double open_profit_loss;
        public byte prefer_order_book_l1;
        public byte existing;
        public byte read_only_shadow;
        public byte reserved0;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = SymbolCapacity)]
        public byte[]? symbol;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = BoardCapacity)]
        public byte[]? board;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = ExchangeCapacity)]
        public byte[]? exchange;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = GroupCapacity)]
        public byte[]? instrument_group;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = PortfolioCapacity)]
        public byte[]? portfolio;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = AccountCapacity)]
        public byte[]? trade_account;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = OrderIdCapacity)]
        public byte[]? server_order_id;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = OrderIdCapacity)]
        public byte[]? client_order_id;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = InfoCapacity)]
        public byte[]? info_text;
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate IntPtr AbiNameFn();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate uint AbiVersionFn();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate byte EnvironmentStartAllowedFn(IntPtr environment, byte armed);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate byte ProdRequiresExplicitArmFn();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate uint SizeofFn();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate uint AlignofFn();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate MoexResult CreateConnectorFn(ref NativeMoexConnectorCreateParams request, out IntPtr handle);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate MoexResult DestroyConnectorFn(IntPtr handle);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate MoexResult LoadProfileFn(IntPtr handle, ref NativeMoexProfileLoadParams request);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate MoexResult LoadSyntheticReplayFn(IntPtr handle, IntPtr replayPath);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate MoexResult StartStopFn(IntPtr handle);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate MoexResult PollEventsFn(IntPtr handle, IntPtr outEvents, uint capacity, out uint written);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate MoexResult PollEventsV2Fn(IntPtr handle, IntPtr outEvents, uint eventStrideBytes, uint capacity, out uint written);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void NativeLowRateCallbackFn(IntPtr header, IntPtr payload, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate MoexResult RegisterLowRateCallbackFn(IntPtr handle, NativeLowRateCallbackFn? callback, IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate MoexResult GetHealthFn(IntPtr handle, ref NativeMoexHealthSnapshot buffer);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate MoexResult GetBackpressureFn(IntPtr handle, out NativeMoexBackpressureCounters counters);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate MoexResult FlushRecoveryStateFn(IntPtr handle);
}
