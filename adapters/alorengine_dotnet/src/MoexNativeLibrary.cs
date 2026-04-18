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
        EnvironmentStartAllowed = GetExport<MoexNativeInterop.EnvironmentStartAllowedFn>("moex_environment_start_allowed");
        ProdRequiresExplicitArm = GetExport<MoexNativeInterop.ProdRequiresExplicitArmFn>("moex_prod_requires_explicit_arm");
        SizeofEventHeader = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_event_header");
        SizeofBackpressure = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_backpressure_counters");
        SizeofHealth = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_health_snapshot");
        SizeofConnectorCreateParams = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_connector_create_params");
        SizeofProfileLoadParams = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_profile_load_params");
        SizeofOrderSubmitRequest = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_order_submit_request");
        SizeofOrderCancelRequest = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_order_cancel_request");
        SizeofOrderReplaceRequest = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_order_replace_request");
        SizeofMassCancelRequest = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_mass_cancel_request");
        SizeofSubscriptionRequest = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_subscription_request");
        SizeofPolledEvent = GetExport<MoexNativeInterop.SizeofFn>("moex_sizeof_polled_event");
        AlignofEventHeader = GetExport<MoexNativeInterop.AlignofFn>("moex_alignof_event_header");
        AlignofBackpressure = GetExport<MoexNativeInterop.AlignofFn>("moex_alignof_backpressure_counters");
        AlignofHealth = GetExport<MoexNativeInterop.AlignofFn>("moex_alignof_health_snapshot");
        AlignofConnectorCreateParams = GetExport<MoexNativeInterop.AlignofFn>("moex_alignof_connector_create_params");
        AlignofProfileLoadParams = GetExport<MoexNativeInterop.AlignofFn>("moex_alignof_profile_load_params");
        AlignofOrderSubmitRequest = GetExport<MoexNativeInterop.AlignofFn>("moex_alignof_order_submit_request");
        AlignofOrderCancelRequest = GetExport<MoexNativeInterop.AlignofFn>("moex_alignof_order_cancel_request");
        AlignofOrderReplaceRequest = GetExport<MoexNativeInterop.AlignofFn>("moex_alignof_order_replace_request");
        AlignofMassCancelRequest = GetExport<MoexNativeInterop.AlignofFn>("moex_alignof_mass_cancel_request");
        AlignofSubscriptionRequest = GetExport<MoexNativeInterop.AlignofFn>("moex_alignof_subscription_request");
        AlignofPolledEvent = GetExport<MoexNativeInterop.AlignofFn>("moex_alignof_polled_event");
        CreateConnector = GetExport<MoexNativeInterop.CreateConnectorFn>("moex_create_connector");
        DestroyConnector = GetExport<MoexNativeInterop.DestroyConnectorFn>("moex_destroy_connector");
        LoadProfile = GetExport<MoexNativeInterop.LoadProfileFn>("moex_load_profile");
        LoadSyntheticReplay = GetExport<MoexNativeInterop.LoadSyntheticReplayFn>("moex_load_synthetic_replay");
        StartConnector = GetExport<MoexNativeInterop.StartStopFn>("moex_start_connector");
        StopConnector = GetExport<MoexNativeInterop.StartStopFn>("moex_stop_connector");
        PollEventsV2 = GetExport<MoexNativeInterop.PollEventsV2Fn>("moex_poll_events_v2");
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

    public MoexAbiLayout Layout { get; private set; } = new(Array.Empty<MoexAbiStructLayout>());

    internal MoexNativeInterop.EnvironmentStartAllowedFn EnvironmentStartAllowed { get; }
    internal MoexNativeInterop.ProdRequiresExplicitArmFn ProdRequiresExplicitArm { get; }
    internal MoexNativeInterop.SizeofFn SizeofEventHeader { get; }
    internal MoexNativeInterop.SizeofFn SizeofBackpressure { get; }
    internal MoexNativeInterop.SizeofFn SizeofHealth { get; }
    internal MoexNativeInterop.SizeofFn SizeofConnectorCreateParams { get; }
    internal MoexNativeInterop.SizeofFn SizeofProfileLoadParams { get; }
    internal MoexNativeInterop.SizeofFn SizeofOrderSubmitRequest { get; }
    internal MoexNativeInterop.SizeofFn SizeofOrderCancelRequest { get; }
    internal MoexNativeInterop.SizeofFn SizeofOrderReplaceRequest { get; }
    internal MoexNativeInterop.SizeofFn SizeofMassCancelRequest { get; }
    internal MoexNativeInterop.SizeofFn SizeofSubscriptionRequest { get; }
    internal MoexNativeInterop.SizeofFn SizeofPolledEvent { get; }
    internal MoexNativeInterop.AlignofFn AlignofEventHeader { get; }
    internal MoexNativeInterop.AlignofFn AlignofBackpressure { get; }
    internal MoexNativeInterop.AlignofFn AlignofHealth { get; }
    internal MoexNativeInterop.AlignofFn AlignofConnectorCreateParams { get; }
    internal MoexNativeInterop.AlignofFn AlignofProfileLoadParams { get; }
    internal MoexNativeInterop.AlignofFn AlignofOrderSubmitRequest { get; }
    internal MoexNativeInterop.AlignofFn AlignofOrderCancelRequest { get; }
    internal MoexNativeInterop.AlignofFn AlignofOrderReplaceRequest { get; }
    internal MoexNativeInterop.AlignofFn AlignofMassCancelRequest { get; }
    internal MoexNativeInterop.AlignofFn AlignofSubscriptionRequest { get; }
    internal MoexNativeInterop.AlignofFn AlignofPolledEvent { get; }
    internal MoexNativeInterop.CreateConnectorFn CreateConnector { get; }
    internal MoexNativeInterop.DestroyConnectorFn DestroyConnector { get; }
    internal MoexNativeInterop.LoadProfileFn LoadProfile { get; }
    internal MoexNativeInterop.LoadSyntheticReplayFn LoadSyntheticReplay { get; }
    internal MoexNativeInterop.StartStopFn StartConnector { get; }
    internal MoexNativeInterop.StartStopFn StopConnector { get; }
    internal MoexNativeInterop.PollEventsV2Fn PollEventsV2 { get; }
    internal MoexNativeInterop.PollEventsFn PollEvents { get; }
    internal MoexNativeInterop.RegisterLowRateCallbackFn RegisterLowRateCallback { get; }
    internal MoexNativeInterop.GetHealthFn GetHealth { get; }
    internal MoexNativeInterop.GetBackpressureFn GetBackpressureCounters { get; }
    internal MoexNativeInterop.FlushRecoveryStateFn FlushRecoveryState { get; }

    public static MoexNativeLibrary Load(string libraryPath) => new(libraryPath);

    public bool IsEnvironmentStartAllowed(string environment, bool armed)
    {
        using var env = new NativeUtf8String(environment);
        return EnvironmentStartAllowed(env.Pointer, armed ? (byte)1 : (byte)0) != 0;
    }

    public bool ProductionRequiresExplicitArm() => ProdRequiresExplicitArm() != 0;

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

        Layout = new MoexAbiLayout(
            new[]
            {
                CreateStructLayout(
                    "MoexEventHeader",
                    SizeofEventHeader(),
                    Marshal.SizeOf<MoexNativeInterop.NativeMoexEventHeader>(),
                    AlignofEventHeader(),
                    MoexAbiLayout.AlignmentOf<MoexAbiLayout.EventHeaderProbe>(nameof(MoexAbiLayout.EventHeaderProbe.value))),
                CreateStructLayout(
                    "MoexBackpressureCounters",
                    SizeofBackpressure(),
                    Marshal.SizeOf<MoexNativeInterop.NativeMoexBackpressureCounters>(),
                    AlignofBackpressure(),
                    MoexAbiLayout.AlignmentOf<MoexAbiLayout.BackpressureCountersProbe>(nameof(MoexAbiLayout.BackpressureCountersProbe.value))),
                CreateStructLayout(
                    "MoexHealthSnapshot",
                    SizeofHealth(),
                    Marshal.SizeOf<MoexNativeInterop.NativeMoexHealthSnapshot>(),
                    AlignofHealth(),
                    MoexAbiLayout.AlignmentOf<MoexAbiLayout.HealthSnapshotProbe>(nameof(MoexAbiLayout.HealthSnapshotProbe.value))),
                CreateStructLayout(
                    "MoexConnectorCreateParams",
                    SizeofConnectorCreateParams(),
                    Marshal.SizeOf<MoexNativeInterop.NativeMoexConnectorCreateParams>(),
                    AlignofConnectorCreateParams(),
                    MoexAbiLayout.AlignmentOf<MoexAbiLayout.ConnectorCreateParamsProbe>(nameof(MoexAbiLayout.ConnectorCreateParamsProbe.value))),
                CreateStructLayout(
                    "MoexProfileLoadParams",
                    SizeofProfileLoadParams(),
                    Marshal.SizeOf<MoexNativeInterop.NativeMoexProfileLoadParams>(),
                    AlignofProfileLoadParams(),
                    MoexAbiLayout.AlignmentOf<MoexAbiLayout.ProfileLoadParamsProbe>(nameof(MoexAbiLayout.ProfileLoadParamsProbe.value))),
                CreateStructLayout(
                    "MoexOrderSubmitRequest",
                    SizeofOrderSubmitRequest(),
                    Marshal.SizeOf<MoexNativeInterop.NativeMoexOrderSubmitRequest>(),
                    AlignofOrderSubmitRequest(),
                    MoexAbiLayout.AlignmentOf<MoexAbiLayout.OrderSubmitRequestProbe>(nameof(MoexAbiLayout.OrderSubmitRequestProbe.value))),
                CreateStructLayout(
                    "MoexOrderCancelRequest",
                    SizeofOrderCancelRequest(),
                    Marshal.SizeOf<MoexNativeInterop.NativeMoexOrderCancelRequest>(),
                    AlignofOrderCancelRequest(),
                    MoexAbiLayout.AlignmentOf<MoexAbiLayout.OrderCancelRequestProbe>(nameof(MoexAbiLayout.OrderCancelRequestProbe.value))),
                CreateStructLayout(
                    "MoexOrderReplaceRequest",
                    SizeofOrderReplaceRequest(),
                    Marshal.SizeOf<MoexNativeInterop.NativeMoexOrderReplaceRequest>(),
                    AlignofOrderReplaceRequest(),
                    MoexAbiLayout.AlignmentOf<MoexAbiLayout.OrderReplaceRequestProbe>(nameof(MoexAbiLayout.OrderReplaceRequestProbe.value))),
                CreateStructLayout(
                    "MoexMassCancelRequest",
                    SizeofMassCancelRequest(),
                    Marshal.SizeOf<MoexNativeInterop.NativeMoexMassCancelRequest>(),
                    AlignofMassCancelRequest(),
                    MoexAbiLayout.AlignmentOf<MoexAbiLayout.MassCancelRequestProbe>(nameof(MoexAbiLayout.MassCancelRequestProbe.value))),
                CreateStructLayout(
                    "MoexSubscriptionRequest",
                    SizeofSubscriptionRequest(),
                    Marshal.SizeOf<MoexNativeInterop.NativeMoexSubscriptionRequest>(),
                    AlignofSubscriptionRequest(),
                    MoexAbiLayout.AlignmentOf<MoexAbiLayout.SubscriptionRequestProbe>(nameof(MoexAbiLayout.SubscriptionRequestProbe.value))),
                CreateStructLayout(
                    "MoexPolledEvent",
                    SizeofPolledEvent(),
                    Marshal.SizeOf<MoexNativeInterop.NativeMoexPolledEvent>(),
                    AlignofPolledEvent(),
                    MoexAbiLayout.AlignmentOf<MoexAbiLayout.PolledEventProbe>(nameof(MoexAbiLayout.PolledEventProbe.value)))
            });
    }

    private T GetExport<T>(string name) where T : Delegate
    {
        var symbol = NativeLibrary.GetExport(_libraryHandle, name);
        return Marshal.GetDelegateForFunctionPointer<T>(symbol);
    }

    private static MoexAbiStructLayout CreateStructLayout(
        string structName,
        uint nativeSize,
        int managedSize,
        uint nativeAlignment,
        int managedAlignment)
    {
        if (nativeSize != managedSize)
        {
            throw new InvalidOperationException($"{structName} size mismatch: native={nativeSize}, managed={managedSize}.");
        }

        if (nativeAlignment != managedAlignment)
        {
            throw new InvalidOperationException($"{structName} alignment mismatch: native={nativeAlignment}, managed={managedAlignment}.");
        }

        return new MoexAbiStructLayout(structName, nativeSize, managedSize, nativeAlignment, managedAlignment);
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
