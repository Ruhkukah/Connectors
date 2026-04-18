using System.Runtime.InteropServices;

namespace MoexConnector.AlorEngine;

public sealed class MoexNativeConnector : IDisposable
{
    private readonly MoexNativeLibrary _library;
    private readonly MoexConnectorSafeHandle _handle;
    private MoexNativeInterop.NativeLowRateCallbackFn? _nativeLowRateCallback;
    private Action<MoexConnectorEvent>? _managedLowRateCallback;
    private bool _disposed;

    internal MoexNativeConnector(MoexNativeLibrary library, MoexConnectorSafeHandle handle)
    {
        _library = library;
        _handle = handle;
    }

    public MoexConnectorSafeHandle Handle => _handle;

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _handle.Dispose();
        _disposed = true;
        GC.SuppressFinalize(this);
    }

    public void LoadProfile(string profilePath, bool armed)
    {
        ThrowIfDisposed();
        using var profilePathUtf8 = new MoexNativeLibrary.NativeUtf8String(profilePath);
        var request = new MoexNativeInterop.NativeMoexProfileLoadParams
        {
            struct_size = (uint)Marshal.SizeOf<MoexNativeInterop.NativeMoexProfileLoadParams>(),
            abi_version = MoexNativeInterop.AbiVersion,
            profile_path = profilePathUtf8.Pointer,
            armed = armed ? (byte)1 : (byte)0,
            reserved1 = new byte[7]
        };

        var result = _library.LoadProfile(_handle.DangerousGetHandle(), ref request);
        MoexNativeLibrary.EnsureSuccess("moex_load_profile", result);
    }

    public void LoadSyntheticReplay(string replayPath)
    {
        ThrowIfDisposed();
        using var replayPathUtf8 = new MoexNativeLibrary.NativeUtf8String(replayPath);
        var result = _library.LoadSyntheticReplay(_handle.DangerousGetHandle(), replayPathUtf8.Pointer);
        MoexNativeLibrary.EnsureSuccess("moex_load_synthetic_replay", result);
    }

    public void Start()
    {
        ThrowIfDisposed();
        MoexNativeLibrary.EnsureSuccess("moex_start_connector", _library.StartConnector(_handle.DangerousGetHandle()));
    }

    public void Stop()
    {
        ThrowIfDisposed();
        MoexNativeLibrary.EnsureSuccess("moex_stop_connector", _library.StopConnector(_handle.DangerousGetHandle()));
    }

    public void RegisterLowRateCallback(Action<MoexConnectorEvent> callback)
    {
        ThrowIfDisposed();
        _managedLowRateCallback = callback;
        _nativeLowRateCallback = (_, payload, _) =>
        {
            if (_managedLowRateCallback is null || payload == IntPtr.Zero)
            {
                return;
            }

            var native = Marshal.PtrToStructure<MoexNativeInterop.NativeMoexPolledEvent>(payload);
            _managedLowRateCallback(MoexNativeMapper.Map(native));
        };

        MoexNativeLibrary.EnsureSuccess(
            "moex_register_low_rate_callback",
            _library.RegisterLowRateCallback(_handle.DangerousGetHandle(), _nativeLowRateCallback, IntPtr.Zero));
    }

    public IReadOnlyList<MoexConnectorEvent> PollEvents(int capacity)
    {
        var (events, _, _) = PollEventsInternal(capacity, checked((int)_library.SizeofPolledEvent()), throwOnError: true);
        return events;
    }

    public MoexResult PollEventsWithStrideForTest(int eventStrideBytes, int capacity, out uint written)
    {
        var (_, result, polled) = PollEventsInternal(capacity, eventStrideBytes, throwOnError: false);
        written = polled;
        return result;
    }

    public MoexHealthView GetHealth()
    {
        ThrowIfDisposed();
        var buffer = new MoexNativeInterop.NativeMoexHealthSnapshot
        {
            struct_size = (uint)Marshal.SizeOf<MoexNativeInterop.NativeMoexHealthSnapshot>(),
            abi_version = MoexNativeInterop.AbiVersion,
            reserved1 = new byte[6]
        };
        var result = _library.GetHealth(_handle.DangerousGetHandle(), ref buffer);
        MoexNativeLibrary.EnsureSuccess("moex_get_health", result);
        return MoexNativeMapper.Map(buffer);
    }

    public MoexBackpressureSnapshot GetBackpressureCounters()
    {
        ThrowIfDisposed();
        var result = _library.GetBackpressureCounters(_handle.DangerousGetHandle(), out var counters);
        MoexNativeLibrary.EnsureSuccess("moex_get_backpressure_counters", result);
        return MoexNativeMapper.Map(counters);
    }

    public void FlushRecoveryState()
    {
        ThrowIfDisposed();
        MoexNativeLibrary.EnsureSuccess("moex_flush_recovery_state", _library.FlushRecoveryState(_handle.DangerousGetHandle()));
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        _library.ThrowIfDisposed();
    }

    private (IReadOnlyList<MoexConnectorEvent> Events, MoexResult Result, uint Written) PollEventsInternal(
        int capacity,
        int eventStrideBytes,
        bool throwOnError)
    {
        ThrowIfDisposed();
        if (capacity <= 0)
        {
            return (Array.Empty<MoexConnectorEvent>(), MoexResult.Ok, 0U);
        }

        if (eventStrideBytes <= 0)
        {
            return (Array.Empty<MoexConnectorEvent>(), MoexResult.InvalidArgument, 0U);
        }

        var buffer = Marshal.AllocHGlobal(eventStrideBytes * capacity);
        try
        {
            var result = _library.PollEventsV2(
                _handle.DangerousGetHandle(),
                buffer,
                checked((uint)eventStrideBytes),
                checked((uint)capacity),
                out var written);

            if (throwOnError)
            {
                MoexNativeLibrary.EnsureSuccess("moex_poll_events_v2", result);
            }
            else if (result != MoexResult.Ok)
            {
                return (Array.Empty<MoexConnectorEvent>(), result, written);
            }

            if (written == 0)
            {
                return (Array.Empty<MoexConnectorEvent>(), result, 0U);
            }

            var output = new List<MoexConnectorEvent>((int)written);
            for (var index = 0; index < written; index++)
            {
                var native = Marshal.PtrToStructure<MoexNativeInterop.NativeMoexPolledEvent>(buffer + (index * eventStrideBytes));
                output.Add(MoexNativeMapper.Map(native));
            }

            return (output, result, written);
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }
}
