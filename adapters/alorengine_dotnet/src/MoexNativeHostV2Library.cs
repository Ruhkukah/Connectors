using System.Runtime.InteropServices;

namespace MoexConnector.AlorEngine;

public enum MoexConnectorHostPurposeV2 : ushort
{
    Qualify = 0,
    OrderTest = 1
}

public sealed class MoexNativeHostV2CreateOptions
{
    public MoexConnectorHostPurposeV2 Purpose { get; init; }
    public required string RuntimeRoot { get; init; }
    public string? LibraryPath { get; init; }
    public required string SchemeDirectory { get; init; }
    public required string ConfigDirectory { get; init; }
    public required string EnvironmentSettingsVariable { get; init; }
    public string? CredentialsVariable { get; init; }
    public string? SoftwareKeyVariable { get; init; }
    public required string BrokerCodeVariable { get; init; }
    public required string ClientCodeVariable { get; init; }
    public string? ExpectedRelease { get; init; }
    public string? ExpectedSchemeSha256 { get; init; }
    public long IsinId { get; init; }
    public int SessionId { get; init; }
    public bool ArmedTestNetwork { get; init; }
    public bool ArmedTestSession { get; init; }
    public bool ArmedTestPlaza2 { get; init; }
    public bool ArmedTestOrderSend { get; init; }
    public uint Side { get; init; } = 2;
    public string? Price { get; init; }
    public string? BaseContractCode { get; init; }
    public string? Comment { get; init; }
    public int ExtId { get; init; } = 79;
    public uint AddUserId { get; init; } = 701;
    public uint CancelUserId { get; init; } = 702;
    public uint RecoveryUserId { get; init; } = 703;
    public string? RunId { get; init; }
    public string? JournalRoot { get; init; }
    public string? ReceiptPath { get; init; }
    public string? ProfileId { get; init; }
    public string? ProfileFingerprint { get; init; }
    public string? PolicyVersion { get; init; }
    public string? PolicySha256 { get; init; }
}

public sealed class MoexNativeHostV2Library : IDisposable
{
    private readonly IntPtr _libraryHandle;
    private bool _disposed;

    private MoexNativeHostV2Library(string libraryPath)
    {
        LibraryPath = Path.GetFullPath(libraryPath);
        _libraryHandle = NativeLibrary.Load(LibraryPath);
        AbiVersion = GetExport<MoexNativeInteropV2.AbiVersionFn>("moex_v2_abi_version")();
        CreateHostNative = GetExport<MoexNativeInteropV2.CreateHostFn>("moex_v2_create_host");
        DestroyHost = GetExport<MoexNativeInteropV2.DestroyHostFn>("moex_v2_destroy_host");
        Start = GetExport<MoexNativeInteropV2.HandleFn>("moex_v2_start");
        Poll = GetExport<MoexNativeInteropV2.HandleFn>("moex_v2_poll");
        Stop = GetExport<MoexNativeInteropV2.HandleFn>("moex_v2_stop");
        GetSnapshot = GetExport<MoexNativeInteropV2.GetSnapshotFn>("moex_v2_get_snapshot");
        GetPlanInfo = GetExport<MoexNativeInteropV2.GetPlanInfoFn>("moex_v2_get_plan_info");
        CopyPlanCanonical = GetExport<MoexNativeInteropV2.CopyPlanCanonicalFn>("moex_v2_copy_plan_canonical");
        Authorize = GetExport<MoexNativeInteropV2.AuthorizeFn>("moex_v2_authorize");
        RunOrderTest = GetExport<MoexNativeInteropV2.RunOrderTestFn>("moex_v2_run_order_test");
        Reconcile = GetExport<MoexNativeInteropV2.ReconcileFn>("moex_v2_reconcile");

        SizeofCreateParams = GetExport<MoexNativeInteropV2.SizeofFn>("moex_v2_sizeof_MoexConnectorHostCreateParamsV2");
        AlignofCreateParams = GetExport<MoexNativeInteropV2.AlignofFn>("moex_v2_alignof_MoexConnectorHostCreateParamsV2");
        SizeofStreamHealth = GetExport<MoexNativeInteropV2.SizeofFn>("moex_v2_sizeof_MoexV2StreamHealth");
        AlignofStreamHealth = GetExport<MoexNativeInteropV2.AlignofFn>("moex_v2_alignof_MoexV2StreamHealth");
        SizeofTargetProvenance = GetExport<MoexNativeInteropV2.SizeofFn>("moex_v2_sizeof_MoexV2TargetProvenance");
        AlignofTargetProvenance = GetExport<MoexNativeInteropV2.AlignofFn>("moex_v2_alignof_MoexV2TargetProvenance");
        SizeofSnapshot = GetExport<MoexNativeInteropV2.SizeofFn>("moex_v2_sizeof_MoexConnectorHostSnapshotV2");
        AlignofSnapshot = GetExport<MoexNativeInteropV2.AlignofFn>("moex_v2_alignof_MoexConnectorHostSnapshotV2");
        SizeofPlanInfo = GetExport<MoexNativeInteropV2.SizeofFn>("moex_v2_sizeof_MoexPreSendPlanInfoV2");
        AlignofPlanInfo = GetExport<MoexNativeInteropV2.AlignofFn>("moex_v2_alignof_MoexPreSendPlanInfoV2");
        SizeofReplyInfo = GetExport<MoexNativeInteropV2.SizeofFn>("moex_v2_sizeof_MoexV2ReplyInfo");
        AlignofReplyInfo = GetExport<MoexNativeInteropV2.AlignofFn>("moex_v2_alignof_MoexV2ReplyInfo");
        SizeofSubmissionInfo = GetExport<MoexNativeInteropV2.SizeofFn>("moex_v2_sizeof_MoexV2SubmissionInfo");
        AlignofSubmissionInfo = GetExport<MoexNativeInteropV2.AlignofFn>("moex_v2_alignof_MoexV2SubmissionInfo");
        SizeofOrderTestResult = GetExport<MoexNativeInteropV2.SizeofFn>("moex_v2_sizeof_MoexOrderTestResultV2");
        AlignofOrderTestResult = GetExport<MoexNativeInteropV2.AlignofFn>("moex_v2_alignof_MoexOrderTestResultV2");
        SizeofReconciliation = GetExport<MoexNativeInteropV2.SizeofFn>("moex_v2_sizeof_MoexRestartReconciliationResultV2");
        AlignofReconciliation = GetExport<MoexNativeInteropV2.AlignofFn>("moex_v2_alignof_MoexRestartReconciliationResultV2");

        if (AbiVersion != MoexNativeInteropV2.AbiVersion)
        {
            throw new InvalidOperationException($"Unexpected ConnectorHost V2 ABI version {AbiVersion}.");
        }

        Layout = new[]
        {
            ValidateLayout("MoexConnectorHostCreateParamsV2", SizeofCreateParams(), Marshal.SizeOf<MoexNativeInteropV2.NativeCreateParams>(), AlignofCreateParams(), AlignmentOf<CreateProbe>()),
            ValidateLayout("MoexV2StreamHealth", SizeofStreamHealth(), Marshal.SizeOf<MoexNativeInteropV2.NativeStreamHealth>(), AlignofStreamHealth(), AlignmentOf<StreamHealthProbe>()),
            ValidateLayout("MoexV2TargetProvenance", SizeofTargetProvenance(), Marshal.SizeOf<MoexNativeInteropV2.NativeTargetProvenance>(), AlignofTargetProvenance(), AlignmentOf<TargetProvenanceProbe>()),
            ValidateLayout("MoexConnectorHostSnapshotV2", SizeofSnapshot(), Marshal.SizeOf<MoexNativeInteropV2.NativeSnapshot>(), AlignofSnapshot(), AlignmentOf<SnapshotProbe>()),
            ValidateLayout("MoexPreSendPlanInfoV2", SizeofPlanInfo(), Marshal.SizeOf<MoexNativeInteropV2.NativePlanInfo>(), AlignofPlanInfo(), AlignmentOf<PlanInfoProbe>()),
            ValidateLayout("MoexV2ReplyInfo", SizeofReplyInfo(), Marshal.SizeOf<MoexNativeInteropV2.NativeReplyInfo>(), AlignofReplyInfo(), AlignmentOf<ReplyInfoProbe>()),
            ValidateLayout("MoexV2SubmissionInfo", SizeofSubmissionInfo(), Marshal.SizeOf<MoexNativeInteropV2.NativeSubmissionInfo>(), AlignofSubmissionInfo(), AlignmentOf<SubmissionInfoProbe>()),
            ValidateLayout("MoexOrderTestResultV2", SizeofOrderTestResult(), Marshal.SizeOf<MoexNativeInteropV2.NativeOrderTestResult>(), AlignofOrderTestResult(), AlignmentOf<OrderTestResultProbe>()),
            ValidateLayout("MoexRestartReconciliationResultV2", SizeofReconciliation(), Marshal.SizeOf<MoexNativeInteropV2.NativeReconciliationResult>(), AlignofReconciliation(), AlignmentOf<ReconciliationProbe>())
        };
    }

    public string LibraryPath { get; }
    public uint AbiVersion { get; }
    public IReadOnlyList<MoexAbiStructLayout> Layout { get; }

    internal MoexNativeInteropV2.CreateHostFn CreateHostNative { get; }
    internal MoexNativeInteropV2.DestroyHostFn DestroyHost { get; }
    internal MoexNativeInteropV2.HandleFn Start { get; }
    internal MoexNativeInteropV2.HandleFn Poll { get; }
    internal MoexNativeInteropV2.HandleFn Stop { get; }
    internal MoexNativeInteropV2.GetSnapshotFn GetSnapshot { get; }
    internal MoexNativeInteropV2.GetPlanInfoFn GetPlanInfo { get; }
    internal MoexNativeInteropV2.CopyPlanCanonicalFn CopyPlanCanonical { get; }
    internal MoexNativeInteropV2.AuthorizeFn Authorize { get; }
    internal MoexNativeInteropV2.RunOrderTestFn RunOrderTest { get; }
    internal MoexNativeInteropV2.ReconcileFn Reconcile { get; }

    private MoexNativeInteropV2.SizeofFn SizeofCreateParams { get; }
    private MoexNativeInteropV2.AlignofFn AlignofCreateParams { get; }
    private MoexNativeInteropV2.SizeofFn SizeofStreamHealth { get; }
    private MoexNativeInteropV2.AlignofFn AlignofStreamHealth { get; }
    private MoexNativeInteropV2.SizeofFn SizeofTargetProvenance { get; }
    private MoexNativeInteropV2.AlignofFn AlignofTargetProvenance { get; }
    private MoexNativeInteropV2.SizeofFn SizeofSnapshot { get; }
    private MoexNativeInteropV2.AlignofFn AlignofSnapshot { get; }
    private MoexNativeInteropV2.SizeofFn SizeofPlanInfo { get; }
    private MoexNativeInteropV2.AlignofFn AlignofPlanInfo { get; }
    private MoexNativeInteropV2.SizeofFn SizeofReplyInfo { get; }
    private MoexNativeInteropV2.AlignofFn AlignofReplyInfo { get; }
    private MoexNativeInteropV2.SizeofFn SizeofSubmissionInfo { get; }
    private MoexNativeInteropV2.AlignofFn AlignofSubmissionInfo { get; }
    private MoexNativeInteropV2.SizeofFn SizeofOrderTestResult { get; }
    private MoexNativeInteropV2.AlignofFn AlignofOrderTestResult { get; }
    private MoexNativeInteropV2.SizeofFn SizeofReconciliation { get; }
    private MoexNativeInteropV2.AlignofFn AlignofReconciliation { get; }

    public static MoexNativeHostV2Library Load(string libraryPath) => new(libraryPath);

    public MoexNativeHostV2Handle CreateHost(MoexNativeHostV2CreateOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        ThrowIfDisposed();
        using var arena = new NativeUtf8Arena();
        var request = new MoexNativeInteropV2.NativeCreateParams
        {
            struct_size = (uint)Marshal.SizeOf<MoexNativeInteropV2.NativeCreateParams>(),
            abi_version = MoexNativeInteropV2.AbiVersion,
            purpose = (ushort)options.Purpose,
            runtime_root = arena.Add(options.RuntimeRoot),
            library_path = arena.Add(options.LibraryPath),
            scheme_dir = arena.Add(options.SchemeDirectory),
            config_dir = arena.Add(options.ConfigDirectory),
            env_settings_env_var = arena.Add(options.EnvironmentSettingsVariable),
            credentials_env_var = arena.Add(options.CredentialsVariable),
            software_key_env_var = arena.Add(options.SoftwareKeyVariable),
            broker_code_env_var = arena.Add(options.BrokerCodeVariable),
            client_code_env_var = arena.Add(options.ClientCodeVariable),
            expected_release = arena.Add(options.ExpectedRelease),
            expected_scheme_sha256 = arena.Add(options.ExpectedSchemeSha256),
            isin_id = options.IsinId,
            session_id = options.SessionId,
            armed_test_network = options.ArmedTestNetwork ? (byte)1 : (byte)0,
            armed_test_session = options.ArmedTestSession ? (byte)1 : (byte)0,
            armed_test_plaza2 = options.ArmedTestPlaza2 ? (byte)1 : (byte)0,
            armed_test_order_send = options.ArmedTestOrderSend ? (byte)1 : (byte)0,
            reserved_flags = new byte[4],
            side = options.Side,
            price = arena.Add(options.Price),
            base_contract_code = arena.Add(options.BaseContractCode),
            comment = arena.Add(options.Comment),
            ext_id = options.ExtId,
            add_user_id = options.AddUserId,
            cancel_user_id = options.CancelUserId,
            recovery_user_id = options.RecoveryUserId,
            run_id = arena.Add(options.RunId),
            journal_root = arena.Add(options.JournalRoot),
            receipt_path = arena.Add(options.ReceiptPath),
            profile_id = arena.Add(options.ProfileId),
            profile_fingerprint = arena.Add(options.ProfileFingerprint),
            policy_version = arena.Add(options.PolicyVersion),
            policy_sha256 = arena.Add(options.PolicySha256),
            reserved = new byte[32]
        };

        var result = CreateHostNative(ref request, out var handle);
        MoexNativeLibrary.EnsureSuccess("moex_v2_create_host", result);
        return new MoexNativeHostV2Handle(this, handle);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        NativeLibrary.Free(_libraryHandle);
        _disposed = true;
        GC.SuppressFinalize(this);
    }

    internal static void EnsureSuccess(string operation, MoexResult result) => MoexNativeLibrary.EnsureSuccess(operation, result);

    private void ThrowIfDisposed() => ObjectDisposedException.ThrowIf(_disposed, this);

    private T GetExport<T>(string name) where T : Delegate
    {
        var symbol = NativeLibrary.GetExport(_libraryHandle, name);
        return Marshal.GetDelegateForFunctionPointer<T>(symbol);
    }

    private static MoexAbiStructLayout ValidateLayout(string name, uint nativeSize, int managedSize, uint nativeAlignment,
                                                       int managedAlignment)
    {
        if (nativeSize != managedSize || nativeAlignment != managedAlignment)
        {
            throw new InvalidOperationException(
                $"{name} V2 layout mismatch: native={nativeSize}/{nativeAlignment}, managed={managedSize}/{managedAlignment}.");
        }

        return new MoexAbiStructLayout(name, nativeSize, managedSize, nativeAlignment, managedAlignment);
    }

    private static int AlignmentOf<TProbe>() where TProbe : struct => Marshal.OffsetOf<TProbe>("value").ToInt32();

    [StructLayout(LayoutKind.Sequential)]
    private struct CreateProbe { public byte prefix; public MoexNativeInteropV2.NativeCreateParams value; }
    [StructLayout(LayoutKind.Sequential)]
    private struct StreamHealthProbe { public byte prefix; public MoexNativeInteropV2.NativeStreamHealth value; }
    [StructLayout(LayoutKind.Sequential)]
    private struct TargetProvenanceProbe { public byte prefix; public MoexNativeInteropV2.NativeTargetProvenance value; }
    [StructLayout(LayoutKind.Sequential)]
    private struct SnapshotProbe { public byte prefix; public MoexNativeInteropV2.NativeSnapshot value; }
    [StructLayout(LayoutKind.Sequential)]
    private struct PlanInfoProbe { public byte prefix; public MoexNativeInteropV2.NativePlanInfo value; }
    [StructLayout(LayoutKind.Sequential)]
    private struct ReplyInfoProbe { public byte prefix; public MoexNativeInteropV2.NativeReplyInfo value; }
    [StructLayout(LayoutKind.Sequential)]
    private struct SubmissionInfoProbe { public byte prefix; public MoexNativeInteropV2.NativeSubmissionInfo value; }
    [StructLayout(LayoutKind.Sequential)]
    private struct OrderTestResultProbe { public byte prefix; public MoexNativeInteropV2.NativeOrderTestResult value; }
    [StructLayout(LayoutKind.Sequential)]
    private struct ReconciliationProbe { public byte prefix; public MoexNativeInteropV2.NativeReconciliationResult value; }

    internal sealed class NativeUtf8Arena : IDisposable
    {
        private readonly List<IntPtr> _pointers = new();

        public IntPtr Add(string? value)
        {
            if (string.IsNullOrEmpty(value))
            {
                return IntPtr.Zero;
            }

            var pointer = Marshal.StringToCoTaskMemUTF8(value);
            _pointers.Add(pointer);
            return pointer;
        }

        public void Dispose()
        {
            foreach (var pointer in _pointers)
            {
                Marshal.FreeCoTaskMem(pointer);
            }

            _pointers.Clear();
        }
    }
}
