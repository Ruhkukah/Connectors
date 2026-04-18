using System.Runtime.InteropServices;

namespace MoexConnector.AlorEngine;

public sealed class MoexConnectorSafeHandle : SafeHandle
{
    private readonly MoexNativeLibrary _owner;
    private readonly MoexNativeInterop.DestroyConnectorFn _destroyConnector;

    internal MoexConnectorSafeHandle(MoexNativeLibrary owner, MoexNativeInterop.DestroyConnectorFn destroyConnector, IntPtr handle)
        : base(IntPtr.Zero, ownsHandle: true)
    {
        _owner = owner;
        _destroyConnector = destroyConnector;
        SetHandle(handle);
    }

    public override bool IsInvalid => handle == IntPtr.Zero;

    protected override bool ReleaseHandle()
    {
        var result = _destroyConnector(handle);
        _owner.NotifyConnectorReleased();
        return result == MoexResult.Ok;
    }
}
