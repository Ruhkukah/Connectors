using System.Runtime.InteropServices;
using System.Threading;

namespace MoexConnector.AlorEngine;

public sealed class MoexNativePersistentHostCreateOptions
{
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
    public int BaseExtId { get; init; }
    public uint BaseAddUserId { get; init; }
    public uint BaseCancelUserId { get; init; }
    public uint BaseRecoveryUserId { get; init; }
    public required string BaseRunId { get; init; }
    public required string JournalRoot { get; init; }
    public required string ReceiptPath { get; init; }
    public required string ProfileId { get; init; }
    public required string ProfileFingerprint { get; init; }
    public string? PolicyVersion { get; init; }
    public string? PolicySha256 { get; init; }
}

public sealed class MoexNativePersistentHostLibrary : IDisposable
{
    private readonly IntPtr _libraryHandle;
    private int _hostCount;
    private bool _disposed;

    private MoexNativePersistentHostLibrary(string libraryPath)
    {
        LibraryPath = Path.GetFullPath(libraryPath);
        _libraryHandle = NativeLibrary.Load(LibraryPath);
        AbiVersion = GetExport<MoexNativeInteropV3.AbiVersionFn>("moex_v3_abi_version")();
        CreateHostNative = GetExport<MoexNativeInteropV3.CreateHostFn>("moex_v3_create_host");
        DestroyHost = GetExport<MoexNativeInteropV3.DestroyHostFn>("moex_v3_destroy_host");
        Start = GetExport<MoexNativeInteropV3.HandleFn>("moex_v3_start");
        Poll = GetExport<MoexNativeInteropV3.HandleFn>("moex_v3_poll");
        Stop = GetExport<MoexNativeInteropV3.HandleFn>("moex_v3_stop");
        GetSnapshot = GetExport<MoexNativeInteropV3.GetSnapshotFn>("moex_v3_get_snapshot");
        PlanOrder = GetExport<MoexNativeInteropV3.PlanOrderFn>("moex_v3_plan_order");
        CopyPlanCanonical = GetExport<MoexNativeInteropV3.CopyPlanCanonicalFn>("moex_v3_copy_plan_canonical");
        BeginOrder = GetExport<MoexNativeInteropV3.BeginOrderFn>("moex_v3_begin_order");
        SubmitOrder = GetExport<MoexNativeInteropV3.OrderResultFn>("moex_v3_submit_order");
        PollOrder = GetExport<MoexNativeInteropV3.OrderResultFn>("moex_v3_poll_order");
        CancelCurrentOrder = GetExport<MoexNativeInteropV3.OrderResultFn>("moex_v3_cancel_current_order");
        FinishOrderEpoch = GetExport<MoexNativeInteropV3.HandleFn>("moex_v3_finish_order_epoch");
        Reconcile = GetExport<MoexNativeInteropV3.ReconcileFn>("moex_v3_reconcile");

        SizeofCreateParams = GetExport<MoexNativeInteropV3.SizeofFn>("moex_v3_sizeof_MoexConnectorHostCreateParamsV3");
        AlignofCreateParams = GetExport<MoexNativeInteropV3.AlignofFn>("moex_v3_alignof_MoexConnectorHostCreateParamsV3");
        SizeofRequest = GetExport<MoexNativeInteropV3.SizeofFn>("moex_v3_sizeof_MoexPersistentOrderRequestV3");
        AlignofRequest = GetExport<MoexNativeInteropV3.AlignofFn>("moex_v3_alignof_MoexPersistentOrderRequestV3");
        SizeofStreamHealth = GetExport<MoexNativeInteropV3.SizeofFn>("moex_v3_sizeof_MoexV3StreamHealth");
        AlignofStreamHealth = GetExport<MoexNativeInteropV3.AlignofFn>("moex_v3_alignof_MoexV3StreamHealth");
        SizeofProvenance = GetExport<MoexNativeInteropV3.SizeofFn>("moex_v3_sizeof_MoexV3TargetProvenance");
        AlignofProvenance = GetExport<MoexNativeInteropV3.AlignofFn>("moex_v3_alignof_MoexV3TargetProvenance");
        SizeofReply = GetExport<MoexNativeInteropV3.SizeofFn>("moex_v3_sizeof_MoexV3ReplyInfo");
        AlignofReply = GetExport<MoexNativeInteropV3.AlignofFn>("moex_v3_alignof_MoexV3ReplyInfo");
        SizeofSubmission = GetExport<MoexNativeInteropV3.SizeofFn>("moex_v3_sizeof_MoexV3SubmissionInfo");
        AlignofSubmission = GetExport<MoexNativeInteropV3.AlignofFn>("moex_v3_alignof_MoexV3SubmissionInfo");
        SizeofSnapshot = GetExport<MoexNativeInteropV3.SizeofFn>("moex_v3_sizeof_MoexPersistentSnapshotV3");
        AlignofSnapshot = GetExport<MoexNativeInteropV3.AlignofFn>("moex_v3_alignof_MoexPersistentSnapshotV3");
        SizeofPlan = GetExport<MoexNativeInteropV3.SizeofFn>("moex_v3_sizeof_MoexPersistentPlanInfoV3");
        AlignofPlan = GetExport<MoexNativeInteropV3.AlignofFn>("moex_v3_alignof_MoexPersistentPlanInfoV3");
        SizeofOrderResult = GetExport<MoexNativeInteropV3.SizeofFn>("moex_v3_sizeof_MoexPersistentOrderResultV3");
        AlignofOrderResult = GetExport<MoexNativeInteropV3.AlignofFn>("moex_v3_alignof_MoexPersistentOrderResultV3");
        SizeofReconciliation = GetExport<MoexNativeInteropV3.SizeofFn>("moex_v3_sizeof_MoexPersistentReconciliationResultV3");
        AlignofReconciliation = GetExport<MoexNativeInteropV3.AlignofFn>("moex_v3_alignof_MoexPersistentReconciliationResultV3");

        if (AbiVersion != MoexNativeInteropV3.AbiVersion)
            throw new InvalidOperationException($"Unexpected ConnectorHost V3 ABI version {AbiVersion}.");
        Layout = new[]
        {
            ValidateLayout("MoexConnectorHostCreateParamsV3", SizeofCreateParams(), Marshal.SizeOf<MoexNativeInteropV3.NativeCreateParams>(), AlignofCreateParams(), AlignmentOf<CreateProbe>()),
            ValidateLayout("MoexPersistentOrderRequestV3", SizeofRequest(), Marshal.SizeOf<MoexNativeInteropV3.NativeOrderRequest>(), AlignofRequest(), AlignmentOf<RequestProbe>()),
            ValidateLayout("MoexV3StreamHealth", SizeofStreamHealth(), Marshal.SizeOf<MoexNativeInteropV3.NativeStreamHealth>(), AlignofStreamHealth(), AlignmentOf<StreamProbe>()),
            ValidateLayout("MoexV3TargetProvenance", SizeofProvenance(), Marshal.SizeOf<MoexNativeInteropV3.NativeTargetProvenance>(), AlignofProvenance(), AlignmentOf<ProvenanceProbe>()),
            ValidateLayout("MoexV3ReplyInfo", SizeofReply(), Marshal.SizeOf<MoexNativeInteropV3.NativeReplyInfo>(), AlignofReply(), AlignmentOf<ReplyProbe>()),
            ValidateLayout("MoexV3SubmissionInfo", SizeofSubmission(), Marshal.SizeOf<MoexNativeInteropV3.NativeSubmissionInfo>(), AlignofSubmission(), AlignmentOf<SubmissionProbe>()),
            ValidateLayout("MoexPersistentSnapshotV3", SizeofSnapshot(), Marshal.SizeOf<MoexNativeInteropV3.NativeSnapshot>(), AlignofSnapshot(), AlignmentOf<SnapshotProbe>()),
            ValidateLayout("MoexPersistentPlanInfoV3", SizeofPlan(), Marshal.SizeOf<MoexNativeInteropV3.NativePlanInfo>(), AlignofPlan(), AlignmentOf<PlanProbe>()),
            ValidateLayout("MoexPersistentOrderResultV3", SizeofOrderResult(), Marshal.SizeOf<MoexNativeInteropV3.NativeOrderResult>(), AlignofOrderResult(), AlignmentOf<OrderResultProbe>()),
            ValidateLayout("MoexPersistentReconciliationResultV3", SizeofReconciliation(), Marshal.SizeOf<MoexNativeInteropV3.NativeReconciliationResult>(), AlignofReconciliation(), AlignmentOf<ReconciliationProbe>())
        };
    }

    public string LibraryPath { get; }
    public uint AbiVersion { get; }
    public IReadOnlyList<MoexAbiStructLayout> Layout { get; }
    internal MoexNativeInteropV3.CreateHostFn CreateHostNative { get; }
    internal MoexNativeInteropV3.DestroyHostFn DestroyHost { get; }
    internal MoexNativeInteropV3.HandleFn Start { get; }
    internal MoexNativeInteropV3.HandleFn Poll { get; }
    internal MoexNativeInteropV3.HandleFn Stop { get; }
    internal MoexNativeInteropV3.GetSnapshotFn GetSnapshot { get; }
    internal MoexNativeInteropV3.PlanOrderFn PlanOrder { get; }
    internal MoexNativeInteropV3.CopyPlanCanonicalFn CopyPlanCanonical { get; }
    internal MoexNativeInteropV3.BeginOrderFn BeginOrder { get; }
    internal MoexNativeInteropV3.OrderResultFn SubmitOrder { get; }
    internal MoexNativeInteropV3.OrderResultFn PollOrder { get; }
    internal MoexNativeInteropV3.OrderResultFn CancelCurrentOrder { get; }
    internal MoexNativeInteropV3.HandleFn FinishOrderEpoch { get; }
    internal MoexNativeInteropV3.ReconcileFn Reconcile { get; }

    private MoexNativeInteropV3.SizeofFn SizeofCreateParams { get; }
    private MoexNativeInteropV3.AlignofFn AlignofCreateParams { get; }
    private MoexNativeInteropV3.SizeofFn SizeofRequest { get; }
    private MoexNativeInteropV3.AlignofFn AlignofRequest { get; }
    private MoexNativeInteropV3.SizeofFn SizeofStreamHealth { get; }
    private MoexNativeInteropV3.AlignofFn AlignofStreamHealth { get; }
    private MoexNativeInteropV3.SizeofFn SizeofProvenance { get; }
    private MoexNativeInteropV3.AlignofFn AlignofProvenance { get; }
    private MoexNativeInteropV3.SizeofFn SizeofReply { get; }
    private MoexNativeInteropV3.AlignofFn AlignofReply { get; }
    private MoexNativeInteropV3.SizeofFn SizeofSubmission { get; }
    private MoexNativeInteropV3.AlignofFn AlignofSubmission { get; }
    private MoexNativeInteropV3.SizeofFn SizeofSnapshot { get; }
    private MoexNativeInteropV3.AlignofFn AlignofSnapshot { get; }
    private MoexNativeInteropV3.SizeofFn SizeofPlan { get; }
    private MoexNativeInteropV3.AlignofFn AlignofPlan { get; }
    private MoexNativeInteropV3.SizeofFn SizeofOrderResult { get; }
    private MoexNativeInteropV3.AlignofFn AlignofOrderResult { get; }
    private MoexNativeInteropV3.SizeofFn SizeofReconciliation { get; }
    private MoexNativeInteropV3.AlignofFn AlignofReconciliation { get; }

    public static MoexNativePersistentHostLibrary Load(string libraryPath) => new(libraryPath);

    public MoexNativePersistentHostHandle CreateHost(MoexNativePersistentHostCreateOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        ThrowIfDisposed();
        ValidateCreateOptions(options);
        using var arena = new NativeUtf8Arena();
        var request = new MoexNativeInteropV3.NativeCreateParams
        {
            struct_size = (uint)Marshal.SizeOf<MoexNativeInteropV3.NativeCreateParams>(), abi_version = MoexNativeInteropV3.AbiVersion,
            runtime_root = arena.Add(options.RuntimeRoot), library_path = arena.Add(options.LibraryPath),
            scheme_dir = arena.Add(options.SchemeDirectory), config_dir = arena.Add(options.ConfigDirectory),
            env_settings_env_var = arena.Add(options.EnvironmentSettingsVariable), credentials_env_var = arena.Add(options.CredentialsVariable),
            software_key_env_var = arena.Add(options.SoftwareKeyVariable), broker_code_env_var = arena.Add(options.BrokerCodeVariable),
            client_code_env_var = arena.Add(options.ClientCodeVariable), expected_release = arena.Add(options.ExpectedRelease),
            expected_scheme_sha256 = arena.Add(options.ExpectedSchemeSha256), isin_id = options.IsinId, session_id = options.SessionId,
            armed_test_network = options.ArmedTestNetwork ? (byte)1 : (byte)0, armed_test_session = options.ArmedTestSession ? (byte)1 : (byte)0,
            armed_test_plaza2 = options.ArmedTestPlaza2 ? (byte)1 : (byte)0, armed_test_order_send = options.ArmedTestOrderSend ? (byte)1 : (byte)0,
            reserved_flags = new byte[4], base_ext_id = options.BaseExtId, base_add_user_id = options.BaseAddUserId,
            base_cancel_user_id = options.BaseCancelUserId, base_recovery_user_id = options.BaseRecoveryUserId,
            base_run_id = arena.Add(options.BaseRunId), journal_root = arena.Add(options.JournalRoot), receipt_path = arena.Add(options.ReceiptPath),
            profile_id = arena.Add(options.ProfileId), profile_fingerprint = arena.Add(options.ProfileFingerprint),
            policy_version = arena.Add(options.PolicyVersion), policy_sha256 = arena.Add(options.PolicySha256), reserved = new byte[32]
        };
        var result = CreateHostNative(ref request, out var handle);
        EnsureSuccess("moex_v3_create_host", result);
        Interlocked.Increment(ref _hostCount);
        return new MoexNativePersistentHostHandle(this, handle);
    }

    public void Dispose()
    {
        if (_disposed) return;
        if (Volatile.Read(ref _hostCount) != 0)
            throw new InvalidOperationException("Dispose all V3 host handles before disposing the native library.");
        NativeLibrary.Free(_libraryHandle); _disposed = true; GC.SuppressFinalize(this);
    }

    internal void NotifyHostReleased() => Interlocked.Decrement(ref _hostCount);
    internal static void EnsureSuccess(string operation, MoexResult result) => MoexNativeLibrary.EnsureSuccess(operation, result);
    internal void ThrowIfDisposed() => ObjectDisposedException.ThrowIf(_disposed, this);

    private static void ValidateCreateOptions(MoexNativePersistentHostCreateOptions options)
    {
        if (options.IsinId <= 0 || options.SessionId <= 0 || options.BaseExtId <= 0 || options.BaseAddUserId == 0 ||
            options.BaseCancelUserId == 0 || options.BaseRecoveryUserId == 0)
            throw new ArgumentException("V3 requires positive target and base identifiers.", nameof(options));
        Require(options.RuntimeRoot, nameof(options.RuntimeRoot)); Require(options.SchemeDirectory, nameof(options.SchemeDirectory));
        Require(options.ConfigDirectory, nameof(options.ConfigDirectory)); Require(options.EnvironmentSettingsVariable, nameof(options.EnvironmentSettingsVariable));
        Require(options.BrokerCodeVariable, nameof(options.BrokerCodeVariable)); Require(options.ClientCodeVariable, nameof(options.ClientCodeVariable));
        Require(options.BaseRunId, nameof(options.BaseRunId)); Require(options.JournalRoot, nameof(options.JournalRoot));
        Require(options.ReceiptPath, nameof(options.ReceiptPath)); Require(options.ProfileId, nameof(options.ProfileId));
        Require(options.ProfileFingerprint, nameof(options.ProfileFingerprint));
    }

    private static void Require(string? value, string name)
    {
        if (string.IsNullOrEmpty(value)) throw new ArgumentException("V3 requires this field.", name);
    }

    private T GetExport<T>(string name) where T : Delegate => Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(_libraryHandle, name));
    private static MoexAbiStructLayout ValidateLayout(string name, uint nativeSize, int managedSize, uint nativeAlignment, int managedAlignment)
    {
        if (nativeSize != managedSize || nativeAlignment != managedAlignment)
            throw new InvalidOperationException($"{name} V3 layout mismatch: native={nativeSize}/{nativeAlignment}, managed={managedSize}/{managedAlignment}.");
        return new(name, nativeSize, managedSize, nativeAlignment, managedAlignment);
    }
    private static int AlignmentOf<TProbe>() where TProbe : struct => Marshal.OffsetOf<TProbe>("value").ToInt32();
    [StructLayout(LayoutKind.Sequential)] private struct CreateProbe { public byte prefix; public MoexNativeInteropV3.NativeCreateParams value; }
    [StructLayout(LayoutKind.Sequential)] private struct RequestProbe { public byte prefix; public MoexNativeInteropV3.NativeOrderRequest value; }
    [StructLayout(LayoutKind.Sequential)] private struct StreamProbe { public byte prefix; public MoexNativeInteropV3.NativeStreamHealth value; }
    [StructLayout(LayoutKind.Sequential)] private struct ProvenanceProbe { public byte prefix; public MoexNativeInteropV3.NativeTargetProvenance value; }
    [StructLayout(LayoutKind.Sequential)] private struct ReplyProbe { public byte prefix; public MoexNativeInteropV3.NativeReplyInfo value; }
    [StructLayout(LayoutKind.Sequential)] private struct SubmissionProbe { public byte prefix; public MoexNativeInteropV3.NativeSubmissionInfo value; }
    [StructLayout(LayoutKind.Sequential)] private struct SnapshotProbe { public byte prefix; public MoexNativeInteropV3.NativeSnapshot value; }
    [StructLayout(LayoutKind.Sequential)] private struct PlanProbe { public byte prefix; public MoexNativeInteropV3.NativePlanInfo value; }
    [StructLayout(LayoutKind.Sequential)] private struct OrderResultProbe { public byte prefix; public MoexNativeInteropV3.NativeOrderResult value; }
    [StructLayout(LayoutKind.Sequential)] private struct ReconciliationProbe { public byte prefix; public MoexNativeInteropV3.NativeReconciliationResult value; }

    internal sealed class NativeUtf8Arena : IDisposable
    {
        private readonly List<IntPtr> _pointers = new();
        public IntPtr Add(string? value)
        {
            if (string.IsNullOrEmpty(value)) return IntPtr.Zero;
            var pointer = Marshal.StringToCoTaskMemUTF8(value); _pointers.Add(pointer); return pointer;
        }
        public void Dispose() { foreach (var pointer in _pointers) Marshal.FreeCoTaskMem(pointer); _pointers.Clear(); }
    }
}
