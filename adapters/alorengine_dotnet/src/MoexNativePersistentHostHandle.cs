using System.Runtime.InteropServices;

namespace MoexConnector.AlorEngine;

public readonly record struct MoexPersistentOrderRequest(uint Side, string Price, string BaseContractCode, string? Comment = null, int Quantity = 1);

public sealed class MoexNativePersistentHostHandle : IDisposable
{
    private readonly MoexNativePersistentHostLibrary _library;
    private IntPtr _handle;

    internal MoexNativePersistentHostHandle(MoexNativePersistentHostLibrary library, IntPtr handle)
    {
        _library = library; _handle = handle;
    }

    public bool IsInvalid => _handle == IntPtr.Zero;

    public void Start() { EnsureHandle(); MoexNativePersistentHostLibrary.EnsureSuccess("moex_v3_start", _library.Start(_handle)); }
    public void Poll() { EnsureHandle(); MoexNativePersistentHostLibrary.EnsureSuccess("moex_v3_poll", _library.Poll(_handle)); }
    public void Stop() { EnsureHandle(); MoexNativePersistentHostLibrary.EnsureSuccess("moex_v3_stop", _library.Stop(_handle)); }

    public MoexNativeInteropV3.NativeSnapshot GetSnapshot()
    {
        EnsureHandle(); var value = MoexNativeInteropV3.NativeSnapshot.Create();
        MoexNativePersistentHostLibrary.EnsureSuccess("moex_v3_get_snapshot", _library.GetSnapshot(_handle, ref value)); return value;
    }

    public MoexNativeInteropV3.NativePlanInfo PlanOrder(MoexPersistentOrderRequest order)
    {
        EnsureHandle(); using var arena = new MoexNativePersistentHostLibrary.NativeUtf8Arena();
        var request = NativeRequest(order, arena); var plan = MoexNativeInteropV3.NativePlanInfo.Create();
        var result = _library.PlanOrder(_handle, ref request, ref plan);
        if (result != MoexResult.Ok && result != MoexResult.SnapshotUnavailable)
            MoexNativePersistentHostLibrary.EnsureSuccess("moex_v3_plan_order", result);
        return plan;
    }

    public byte[] CopyPlanCanonical()
    {
        EnsureHandle();
        var result = _library.CopyPlanCanonical(_handle, IntPtr.Zero, 0, out var required);
        if (result != MoexResult.BufferTooSmall && result != MoexResult.Ok && result != MoexResult.SnapshotUnavailable)
            MoexNativePersistentHostLibrary.EnsureSuccess("moex_v3_copy_plan_canonical", result);
        var bytes = new byte[checked((int)required)]; if (bytes.Length == 0) return bytes;
        var buffer = Marshal.AllocHGlobal(bytes.Length);
        try
        {
            MoexNativePersistentHostLibrary.EnsureSuccess("moex_v3_copy_plan_canonical",
                _library.CopyPlanCanonical(_handle, buffer, checked((uint)bytes.Length), out var written));
            if (written != bytes.Length) throw new InvalidOperationException($"native canonical size changed from {bytes.Length} to {written}");
            Marshal.Copy(buffer, bytes, 0, bytes.Length); return bytes;
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    public void BeginOrder(MoexPersistentOrderRequest order, ReadOnlySpan<byte> canonical, string fullSha256)
    {
        EnsureHandle(); ArgumentNullException.ThrowIfNull(fullSha256);
        if (fullSha256.Length != 64) throw new ArgumentException("authorization SHA must be 64 characters", nameof(fullSha256));
        var canonicalBuffer = canonical.Length == 0 ? IntPtr.Zero : Marshal.AllocHGlobal(canonical.Length);
        try
        {
            if (canonicalBuffer != IntPtr.Zero) Marshal.Copy(canonical.ToArray(), 0, canonicalBuffer, canonical.Length);
            using var arena = new MoexNativePersistentHostLibrary.NativeUtf8Arena();
            var request = NativeRequest(order, arena); var sha = arena.Add(fullSha256);
            MoexNativePersistentHostLibrary.EnsureSuccess("moex_v3_begin_order",
                _library.BeginOrder(_handle, ref request, canonicalBuffer, checked((uint)canonical.Length), sha));
        }
        finally { if (canonicalBuffer != IntPtr.Zero) Marshal.FreeHGlobal(canonicalBuffer); }
    }

    public MoexNativeInteropV3.NativeOrderResult SubmitOrder() => OrderResult("moex_v3_submit_order", _library.SubmitOrder);
    public MoexNativeInteropV3.NativeOrderResult PollOrder() => OrderResult("moex_v3_poll_order", _library.PollOrder);
    public MoexNativeInteropV3.NativeOrderResult CancelCurrentOrder() => OrderResult("moex_v3_cancel_current_order", _library.CancelCurrentOrder);

    public void FinishOrderEpoch() { EnsureHandle(); MoexNativePersistentHostLibrary.EnsureSuccess("moex_v3_finish_order_epoch", _library.FinishOrderEpoch(_handle)); }

    public MoexNativeInteropV3.NativeReconciliationResult Reconcile()
    {
        EnsureHandle(); var value = MoexNativeInteropV3.NativeReconciliationResult.Create();
        MoexNativePersistentHostLibrary.EnsureSuccess("moex_v3_reconcile", _library.Reconcile(_handle, ref value)); return value;
    }

    public void Dispose()
    {
        if (_handle == IntPtr.Zero) return;
        var result = _library.DestroyHost(_handle);
        if (result == MoexResult.Ok) { _handle = IntPtr.Zero; _library.NotifyHostReleased(); }
        MoexNativePersistentHostLibrary.EnsureSuccess("moex_v3_destroy_host", result); GC.SuppressFinalize(this);
    }

    private MoexNativeInteropV3.NativeOrderResult OrderResult(string name, MoexNativeInteropV3.OrderResultFn operation)
    {
        EnsureHandle(); var value = MoexNativeInteropV3.NativeOrderResult.Create();
        MoexNativePersistentHostLibrary.EnsureSuccess(name, operation(_handle, ref value)); return value;
    }

    private static MoexNativeInteropV3.NativeOrderRequest NativeRequest(MoexPersistentOrderRequest order,
                                                                          MoexNativePersistentHostLibrary.NativeUtf8Arena arena) => new()
    {
        struct_size = (uint)Marshal.SizeOf<MoexNativeInteropV3.NativeOrderRequest>(), abi_version = MoexNativeInteropV3.AbiVersion,
        side = order.Side, price = arena.Add(order.Price), base_contract_code = arena.Add(order.BaseContractCode),
        comment = arena.Add(order.Comment), quantity = order.Quantity, reserved = new byte[32]
    };

    private void EnsureHandle()
    {
        _library.ThrowIfDisposed(); ObjectDisposedException.ThrowIf(_handle == IntPtr.Zero, this);
    }
}
