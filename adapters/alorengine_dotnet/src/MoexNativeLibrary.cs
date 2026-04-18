using System.Runtime.InteropServices;
using System.Threading;

namespace MoexConnector.AlorEngine;

public sealed class MoexNativeLibrary : IDisposable
{
    private readonly IntPtr _libraryHandle;
    private int _connectorCount;
    private bool _disposed;

    internal MoexNativeLibrary(string libraryPath)
    {
        LibraryPath = Path.GetFullPath(libraryPath);
        _libraryHandle = NativeLibrary.Load(LibraryPath);

        AbiName = Marshal.PtrToStringUTF8(GetExport<MoexNativeInterop.AbiNameFn>("moex_phase0_abi_name")()) ?? "unknown";
        AbiVersion = GetExport<MoexNativeInterop.AbiVersionFn>("moex_phase0_abi_version")();
        ProdRequiresArm = GetExport<MoexNativeInterop.ProdRequiresArmFn>("moex_phase0_prod_requires_arm");
        SizeofEventHeader = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_event_header");
        SizeofBackpressure = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_backpressure_counters");
        SizeofHealth = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_health_snapshot");
        SizeofConnectorCreateParams = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_connector_create_params");
        SizeofPolledEvent = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_polled_event");
        CreateConnector = GetExport<MoexNativeInterop.CreateConnectorFn>("moex_create_connector");
        DestroyConnector = GetExport<MoexNativeInterop.DestroyConnectorFn>("moex_destroy_connector");
        LoadProfile = GetExport<MoexNativeInterop.LoadProfileFn>("moex_load_profile");
        LoadSyntheticReplay = GetExport<MoexNativeInterop.LoadSyntheticReplayFn>("moex_load_synthetic_replay");
        StartConnector = GetExport<MoexNativeInterop.StartStopFn>("moex_start_connector");
        StopConnector = GetExport<MoexNativeInterop.StartStopFn>("moex_stop_connector");
        PollEvents = GetExport<MoexNativeInterop.PollEventsFn>("moex_poll_events");
        RegisterLowRateCallback = GetExport<MoexNativeInterop.RegisterLowRateCallbackFn>("moex_register_low_rate_callback");
        GetHealth = GetExport<MoexNativeInterop.GetHealthFn>("moex_get_health");
        GetBackpressureCounters = GetExport<MoexNativeInterop.GetBackpressureFn>("moex_get_backpressure_counters");
        FlushRecoveryState = GetExport<MoexNativeInterop.FlushRecoveryStateFn>("moex_flush_recovery_state");

        ValidateAbi();
    }

    public string LibraryPath { get; }

    public string AbiName { get; }

    public uint AbiVersion { get; }

    internal MoexNativeInterop.ProdRequiresArmFn ProdRequiresArm { get; }
    internal MoexNativeInterop.SizeofFn SizeofEventHeader { get; }
    internal MoexNativeInterop.SizeofFn SizeofBackpressure { get; }
    internal MoexNativeInterop.SizeofFn SizeofHealth { get; }
    internal MoexNativeInterop.SizeofFn SizeofConnectorCreateParams { get; }
    internal MoexNativeInterop.SizeofFn SizeofPolledEvent { get; }
    internal MoexNativeInterop.CreateConnectorFn CreateConnector { get; }
    internal MoexNativeInterop.DestroyConnectorFn DestroyConnector { get; }
    internal MoexNativeInterop.LoadProfileFn LoadProfile { get; }
    internal MoexNativeInterop.LoadSyntheticReplayFn LoadSyntheticReplay { get; }
    internal MoexNativeInterop.StartStopFn StartConnector { get; }
    internal MoexNativeInterop.StartStopFn StopConnector { get; }
    internal MoexNativeInterop.PollEventsFn PollEvents { get; }
    internal MoexNativeInterop.RegisterLowRateCallbackFn RegisterLowRateCallback { get; }
    internal MoexNativeInterop.GetHealthFn GetHealth { get; }
    internal MoexNativeInterop.GetBackpressureFn GetBackpressureCounters { get; }
    internal MoexNativeInterop.FlushRecoveryStateFn FlushRecoveryState { get; }

    public static MoexNativeLibrary Load(string libraryPath) => new(libraryPath);

    public bool RequiresExplicitProdArm(string environment, bool armed)
    {
        using var env = new NativeUtf8String(environment);
        return ProdRequiresArm(env.Pointer, armed);
    }

    public MoexNativeConnector CreateConnectorClient(string connectorName, string instanceId)
    {
        ThrowIfDisposed();

        using var connectorNameUtf8 = new NativeUtf8String(connectorName);
        using var instanceIdUtf8 = new NativeUtf8String(instanceId);
        var request = new MoexNativeInterop.NativeMoexConnectorCreateParams
        {
            struct_size = (uint)Marshal.SizeOf<MoexNativeInterop.NativeMoexConnectorCreateParams>(),
            abi_version = MoexNativeInterop.AbiVersion,
            connector_name = connectorNameUtf8.Pointer,
            instance_id = instanceIdUtf8.Pointer
        };

        var result = CreateConnector(ref request, out var handle);
        EnsureSuccess("moex_create_connector", result);
        Interlocked.Increment(ref _connectorCount);
        return new MoexNativeConnector(this, new MoexConnectorSafeHandle(this, DestroyConnector, handle));
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        if (Volatile.Read(ref _connectorCount) != 0)
        {
            throw new InvalidOperationException("Dispose all connector handles before disposing the native library.");
        }

        NativeLibrary.Free(_libraryHandle);
        _disposed = true;
        GC.SuppressFinalize(this);
    }

    internal void NotifyConnectorReleased()
    {
        Interlocked.Decrement(ref _connectorCount);
    }

    internal void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    internal static void EnsureSuccess(string operation, MoexResult result)
    {
        if (result != MoexResult.Ok)
        {
            throw new MoexNativeException(operation, result);
        }
    }

    private void ValidateAbi()
    {
        if (AbiVersion != MoexNativeInterop.AbiVersion)
        {
            throw new InvalidOperationException($"Unexpected ABI version {AbiVersion}.");
        }

        AssertNativeSize(SizeofEventHeader(), Marshal.SizeOf<MoexNativeInterop.NativeMoexEventHeader>(), nameof(MoexNativeInterop.NativeMoexEventHeader));
        AssertNativeSize(SizeofBackpressure(), Marshal.SizeOf<MoexNativeInterop.NativeMoexBackpressureCounters>(), nameof(MoexNativeInterop.NativeMoexBackpressureCounters));
        AssertNativeSize(SizeofHealth(), Marshal.SizeOf<MoexNativeInterop.NativeMoexHealthRequestBuffer>(), nameof(MoexNativeInterop.NativeMoexHealthRequestBuffer));
        AssertNativeSize(SizeofConnectorCreateParams(), Marshal.SizeOf<MoexNativeInterop.NativeMoexConnectorCreateParams>(), nameof(MoexNativeInterop.NativeMoexConnectorCreateParams));
        AssertNativeSize(SizeofPolledEvent(), Marshal.SizeOf<MoexNativeInterop.NativeMoexPolledEvent>(), nameof(MoexNativeInterop.NativeMoexPolledEvent));
    }

    private T GetExport<T>(string name) where T : Delegate
    {
        var symbol = NativeLibrary.GetExport(_libraryHandle, name);
        return Marshal.GetDelegateForFunctionPointer<T>(symbol);
    }

    private static void AssertNativeSize(uint nativeSize, int managedSize, string structName)
    {
        if (nativeSize != managedSize)
        {
            throw new InvalidOperationException($"{structName} size mismatch: native={nativeSize}, managed={managedSize}.");
        }
    }

    internal readonly ref struct NativeUtf8String
    {
        public NativeUtf8String(string value)
        {
            Pointer = Marshal.StringToCoTaskMemUTF8(value);
        }

        public IntPtr Pointer { get; }

        public void Dispose()
        {
            if (Pointer != IntPtr.Zero)
            {
                Marshal.FreeCoTaskMem(Pointer);
            }
        }
    }
}
