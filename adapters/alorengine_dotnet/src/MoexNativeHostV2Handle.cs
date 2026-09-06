using System.Runtime.InteropServices;

namespace MoexConnector.AlorEngine;

public sealed class MoexNativeHostV2Handle : IDisposable
{
    private readonly MoexNativeHostV2Library _library;
    private IntPtr _handle;

    internal MoexNativeHostV2Handle(MoexNativeHostV2Library library, IntPtr handle)
    {
        _library = library;
        _handle = handle;
    }

    public bool IsInvalid => _handle == IntPtr.Zero;

    public void Start()
    {
        EnsureHandle();
        MoexNativeHostV2Library.EnsureSuccess("moex_v2_start", _library.Start(_handle));
    }

    public void Poll()
    {
        EnsureHandle();
        MoexNativeHostV2Library.EnsureSuccess("moex_v2_poll", _library.Poll(_handle));
    }

    public void Stop()
    {
        EnsureHandle();
        MoexNativeHostV2Library.EnsureSuccess("moex_v2_stop", _library.Stop(_handle));
    }

    public MoexNativeInteropV2.NativeSnapshot GetSnapshot()
    {
        EnsureHandle();
        var snapshot = MoexNativeInteropV2.NativeSnapshot.Create();
        MoexNativeHostV2Library.EnsureSuccess("moex_v2_get_snapshot", _library.GetSnapshot(_handle, ref snapshot));
        return snapshot;
    }

    public MoexNativeInteropV2.NativePlanInfo GetPlanInfo()
    {
        EnsureHandle();
        var plan = MoexNativeInteropV2.NativePlanInfo.Create();
        MoexNativeHostV2Library.EnsureSuccess("moex_v2_get_plan_info", _library.GetPlanInfo(_handle, ref plan));
        return plan;
    }

    public byte[] CopyPlanCanonical()
    {
        EnsureHandle();
        var result = _library.CopyPlanCanonical(_handle, IntPtr.Zero, 0, out var required);
        if (result != MoexResult.BufferTooSmall && result != MoexResult.Ok)
        {
            MoexNativeHostV2Library.EnsureSuccess("moex_v2_copy_plan_canonical", result);
        }

        var bytes = new byte[checked((int)required)];
        if (bytes.Length == 0)
        {
            return bytes;
        }

        var buffer = Marshal.AllocHGlobal(bytes.Length);
        try
        {
            MoexNativeHostV2Library.EnsureSuccess(
                "moex_v2_copy_plan_canonical",
                _library.CopyPlanCanonical(_handle, buffer, checked((uint)bytes.Length), out var written));
            if (written != bytes.Length)
            {
                throw new InvalidOperationException($"native canonical size changed from {bytes.Length} to {written}");
            }

            Marshal.Copy(buffer, bytes, 0, bytes.Length);
            return bytes;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    public void Authorize(ReadOnlySpan<byte> canonicalBytes, string fullSha256)
    {
        EnsureHandle();
        ArgumentNullException.ThrowIfNull(fullSha256);
        if (fullSha256.Length != 64)
        {
            throw new ArgumentException("authorization SHA must be the full 64-character digest", nameof(fullSha256));
        }

        var canonicalBuffer = canonicalBytes.Length == 0 ? IntPtr.Zero : Marshal.AllocHGlobal(canonicalBytes.Length);
        try
        {
            if (canonicalBuffer != IntPtr.Zero)
            {
                var bytes = canonicalBytes.ToArray();
                Marshal.Copy(bytes, 0, canonicalBuffer, bytes.Length);
            }

            using var sha = new MoexNativeHostV2Library.NativeUtf8Arena();
            var shaPointer = sha.Add(fullSha256);
            MoexNativeHostV2Library.EnsureSuccess(
                "moex_v2_authorize",
                _library.Authorize(_handle, canonicalBuffer, checked((uint)canonicalBytes.Length), shaPointer));
        }
        finally
        {
            if (canonicalBuffer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(canonicalBuffer);
            }
        }
    }

    public MoexNativeInteropV2.NativeOrderTestResult RunOrderTest()
    {
        EnsureHandle();
        var result = MoexNativeInteropV2.NativeOrderTestResult.Create();
        MoexNativeHostV2Library.EnsureSuccess("moex_v2_run_order_test", _library.RunOrderTest(_handle, ref result));
        return result;
    }

    public MoexNativeInteropV2.NativeReconciliationResult Reconcile()
    {
        EnsureHandle();
        var result = MoexNativeInteropV2.NativeReconciliationResult.Create();
        MoexNativeHostV2Library.EnsureSuccess("moex_v2_reconcile", _library.Reconcile(_handle, ref result));
        return result;
    }

    public void Dispose()
    {
        if (_handle == IntPtr.Zero)
        {
            return;
        }

        var handle = _handle;
        var result = _library.DestroyHost(handle);
        if (result == MoexResult.Ok)
        {
            _handle = IntPtr.Zero;
            _library.NotifyHostReleased();
        }

        MoexNativeHostV2Library.EnsureSuccess("moex_v2_destroy_host", result);
        GC.SuppressFinalize(this);
    }

    private void EnsureHandle()
    {
        _library.ThrowIfDisposed();
        ObjectDisposedException.ThrowIf(_handle == IntPtr.Zero, this);
    }
}
