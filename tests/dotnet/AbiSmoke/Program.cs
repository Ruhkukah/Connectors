using System.Runtime.InteropServices;

if (args.Length != 1)
{
    throw new InvalidOperationException("expected native library path");
}

var libraryPath = Path.GetFullPath(args[0]);
if (!File.Exists(libraryPath))
{
    throw new FileNotFoundException("native library not found", libraryPath);
}

nint libraryHandle = NativeLibrary.Load(libraryPath);
try
{
    var abiVersion = GetExport<AbiVersionFn>(libraryHandle, "moex_phase0_abi_version");
    var createConnector = GetExport<CreateConnectorFn>(libraryHandle, "moex_create_connector");
    var destroyConnector = GetExport<DestroyConnectorFn>(libraryHandle, "moex_destroy_connector");
    var sizeofEventHeader = GetExport<SizeofFn>(libraryHandle, "moex_sizeof_event_header");
    var sizeofBackpressure = GetExport<SizeofFn>(libraryHandle, "moex_sizeof_backpressure_counters");
    var sizeofHealth = GetExport<SizeofFn>(libraryHandle, "moex_sizeof_health_snapshot");
    var sizeofCreateParams = GetExport<SizeofFn>(libraryHandle, "moex_sizeof_connector_create_params");

    if (abiVersion() != 1)
    {
        throw new InvalidOperationException($"unexpected ABI version {abiVersion()}");
    }

    AssertEqual((int)sizeofEventHeader(), Marshal.SizeOf<MoexEventHeader>(), "MoexEventHeader");
    AssertEqual((int)sizeofBackpressure(), Marshal.SizeOf<MoexBackpressureCounters>(), "MoexBackpressureCounters");
    AssertEqual((int)sizeofHealth(), Marshal.SizeOf<MoexHealthSnapshot>(), "MoexHealthSnapshot");
    AssertEqual((int)sizeofCreateParams(), Marshal.SizeOf<MoexConnectorCreateParams>(), "MoexConnectorCreateParams");

    IntPtr connectorName = Marshal.StringToCoTaskMemUTF8("abi-smoke");
    IntPtr instanceId = Marshal.StringToCoTaskMemUTF8("phase0");
    try
    {
        var request = new MoexConnectorCreateParams
        {
            struct_size = (uint)Marshal.SizeOf<MoexConnectorCreateParams>(),
            abi_version = 1,
            connector_name = connectorName,
            instance_id = instanceId,
        };

        var createResult = createConnector(ref request, out nint handle);
        if (createResult != MoexResult.MOEX_RESULT_OK || handle == nint.Zero)
        {
            throw new InvalidOperationException($"create failed: {createResult}");
        }

        var destroyResult = destroyConnector(handle);
        if (destroyResult != MoexResult.MOEX_RESULT_OK)
        {
            throw new InvalidOperationException($"destroy failed: {destroyResult}");
        }
    }
    finally
    {
        Marshal.FreeCoTaskMem(connectorName);
        Marshal.FreeCoTaskMem(instanceId);
    }

    Console.WriteLine("dotnet abi smoke passed");
}
finally
{
    NativeLibrary.Free(libraryHandle);
}

static T GetExport<T>(nint libraryHandle, string name) where T : Delegate
{
    return Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(libraryHandle, name));
}

static void AssertEqual(int nativeSize, int managedSize, string name)
{
    if (nativeSize != managedSize)
    {
        throw new InvalidOperationException($"{name} size mismatch: native={nativeSize} managed={managedSize}");
    }
}

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate uint AbiVersionFn();

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate uint SizeofFn();

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate MoexResult CreateConnectorFn(ref MoexConnectorCreateParams request, out nint handle);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
delegate MoexResult DestroyConnectorFn(nint handle);

enum MoexResult : int
{
    MOEX_RESULT_OK = 0,
    MOEX_RESULT_INVALID_ARGUMENT = 1,
    MOEX_RESULT_NOT_SUPPORTED = 2,
    MOEX_RESULT_NOT_INITIALIZED = 3,
    MOEX_RESULT_ALREADY_STARTED = 4,
    MOEX_RESULT_NOT_STARTED = 5,
    MOEX_RESULT_OVERFLOW = 6,
    MOEX_RESULT_INTERNAL_ERROR = 255,
}

[StructLayout(LayoutKind.Sequential)]
struct MoexConnectorCreateParams
{
    public uint struct_size;
    public ushort abi_version;
    public ushort reserved0;
    public IntPtr connector_name;
    public IntPtr instance_id;
}

[StructLayout(LayoutKind.Sequential)]
struct MoexHealthSnapshot
{
    public uint struct_size;
    public ushort abi_version;
    public ushort reserved0;
    public uint connector_state;
    public uint active_profile_kind;
    public byte prod_armed;
    public byte shadow_mode_enabled;
    public byte reserved1_0;
    public byte reserved1_1;
    public byte reserved1_2;
    public byte reserved1_3;
    public byte reserved1_4;
    public byte reserved1_5;
}

[StructLayout(LayoutKind.Sequential)]
struct MoexEventHeader
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
unsafe struct MoexBackpressureCounters
{
    public ulong produced;
    public ulong polled;
    public ulong dropped;
    public ulong high_watermark;
    public byte overflowed;
    public fixed byte reserved[7];
}
