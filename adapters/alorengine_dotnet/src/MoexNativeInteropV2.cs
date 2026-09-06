using System.Runtime.InteropServices;

namespace MoexConnector.AlorEngine;

public static class MoexNativeInteropV2
{
    public const ushort AbiVersion = 2;
    public const int TextCapacity = 128;
    public const int PathCapacity = 512;
    public const int Sha256Capacity = 65;
    public const int SymbolCapacity = 64;
    public const int MessageCapacity = 256;
    public const int MaxStreams = 16;

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeCreateParams
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort purpose;
        public uint reserved0;
        public IntPtr runtime_root;
        public IntPtr library_path;
        public IntPtr scheme_dir;
        public IntPtr config_dir;
        public IntPtr env_settings_env_var;
        public IntPtr credentials_env_var;
        public IntPtr software_key_env_var;
        public IntPtr broker_code_env_var;
        public IntPtr client_code_env_var;
        public IntPtr expected_release;
        public IntPtr expected_scheme_sha256;
        public long isin_id;
        public int session_id;
        public uint reserved1;
        public byte armed_test_network;
        public byte armed_test_session;
        public byte armed_test_plaza2;
        public byte armed_test_order_send;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)]
        public byte[]? reserved_flags;

        public uint side;
        public IntPtr price;
        public IntPtr base_contract_code;
        public IntPtr comment;
        public int ext_id;
        public uint add_user_id;
        public uint cancel_user_id;
        public uint recovery_user_id;
        public IntPtr run_id;
        public IntPtr journal_root;
        public IntPtr receipt_path;
        public IntPtr profile_id;
        public IntPtr profile_fingerprint;
        public IntPtr policy_version;
        public IntPtr policy_sha256;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
        public byte[]? reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeStreamHealth
    {
        public uint stream_code;
        public byte online;
        public byte snapshot_complete;
        public byte periodic_snapshot_consistent;
        public byte has_publication_state;
        public ulong committed_row_count;
        public ulong last_commit_sequence;
        public int publication_state;
        public int reserved0;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeTargetProvenance
    {
        public uint stream_code;
        public uint table_code;
        public long repl_rev;
        public ulong lifenum;
        public byte present;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 7)]
        public byte[]? reserved;

        public long typed_row_key;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeSnapshot
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort reserved0;
        public uint host_state;
        public uint purpose;
        public uint environment;
        public uint transport_mode;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = TextCapacity)]
        public byte[]? runtime_compatibility;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = Sha256Capacity)]
        public byte[]? runtime_scheme_sha256;

        public byte publisher_ready;
        public byte reply_ready;
        public byte private_streams_ready;
        public byte observation_ready;
        public byte target_refdata_provenance_ready;
        public byte target_aggr20_uncrossed;
        public byte uob_periodic_consistent;
        public byte zero_starting_position_proven;
        public long target_isin_id;
        public int session_id;
        public uint reserved1;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = SymbolCapacity)]
        public byte[]? target;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = TextCapacity)]
        public byte[]? min_step;

        public byte session_status_present;
        public byte instrument_status_present;
        public byte trade_anchor_present;
        public byte lifecycle_state_present;
        public int session_status;
        public int instrument_status;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = TextCapacity)]
        public byte[]? bid;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = TextCapacity)]
        public byte[]? ask;

        public long bbo_age_ms;
        public ulong refdata_lifenum;
        public NativeTargetProvenance fut_instruments_provenance;
        public NativeTargetProvenance fut_sess_contents_provenance;
        public NativeTargetProvenance session_provenance;
        public uint position_evidence_class;
        public long pos_trades_rev;
        public long pos_trades_lifenum;
        public long trade_anchor_trades_rev;
        public long trade_anchor_trades_lifenum;
        public long trade_anchor_server_time;
        public byte trade_replay_complete;
        public byte limits_set;
        public ushort reserved2;
        public ulong active_own_order_count;
        public uint lifecycle_state;
        public uint reserved3;
        public long order_id;
        public long original_quantity;
        public long remaining_quantity;
        public long executed_quantity;
        public byte market_safe;
        public byte evidence_consistent;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 6)]
        public byte[]? reserved_flags;

        public ulong cg_pub_msgnew;
        public ulong cg_pub_post;
        public uint stream_count;
        public uint reserved4;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = MaxStreams)]
        public NativeStreamHealth[]? streams;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = MessageCapacity)]
        public byte[]? last_error;

        public static NativeSnapshot Create() => new()
        {
            struct_size = (uint)Marshal.SizeOf<NativeSnapshot>(),
            abi_version = AbiVersion,
            runtime_compatibility = new byte[TextCapacity],
            runtime_scheme_sha256 = new byte[Sha256Capacity],
            target = new byte[SymbolCapacity],
            min_step = new byte[TextCapacity],
            bid = new byte[TextCapacity],
            ask = new byte[TextCapacity],
            reserved_flags = new byte[6],
            streams = new NativeStreamHealth[MaxStreams],
            last_error = new byte[MessageCapacity]
        };
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativePlanInfo
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort reserved0;
        public byte ok;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
        public byte[]? reserved_flags;

        public uint failure;
        public uint side;
        public long quantity;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = TextCapacity)]
        public byte[]? price;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = Sha256Capacity)]
        public byte[]? plan_sha256;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = Sha256Capacity)]
        public byte[]? add_payload_sha256;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = Sha256Capacity)]
        public byte[]? recovery_payload_sha256;

        public uint canonical_size;
        public uint reserved1;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = MessageCapacity)]
        public byte[]? message;

        public static NativePlanInfo Create() => new()
        {
            struct_size = (uint)Marshal.SizeOf<NativePlanInfo>(),
            abi_version = AbiVersion,
            reserved_flags = new byte[3],
            price = new byte[TextCapacity],
            plan_sha256 = new byte[Sha256Capacity],
            add_payload_sha256 = new byte[Sha256Capacity],
            recovery_payload_sha256 = new byte[Sha256Capacity],
            message = new byte[MessageCapacity]
        };
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeReplyInfo
    {
        public byte present;
        public byte accepted;
        public byte timed_out;
        public byte order_id_present;
        public int code;
        public uint reserved0;
        public long order_id;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeSubmissionInfo
    {
        public uint certainty;
        public byte post_invoked;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
        public byte[]? reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeOrderTestResult
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort reserved0;
        public byte ok;
        public byte market_safe_terminal;
        public byte journal_ok;
        public byte journal_degraded;
        public byte evidence_consistent;
        public byte orphan_incident_written;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)]
        public byte[]? reserved_flags;

        public uint lifecycle_state;
        public long order_id;
        public long original_quantity;
        public long remaining_quantity;
        public long executed_quantity;
        public NativeSubmissionInfo add_submission;
        public NativeSubmissionInfo cancel_submission;
        public NativeSubmissionInfo recovery_submission;
        public NativeReplyInfo add_reply;
        public NativeReplyInfo cancel_reply;
        public NativeReplyInfo recovery_reply;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = PathCapacity)]
        public byte[]? journal_path;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = MessageCapacity)]
        public byte[]? message;

        public static NativeOrderTestResult Create() => new()
        {
            struct_size = (uint)Marshal.SizeOf<NativeOrderTestResult>(),
            abi_version = AbiVersion,
            reserved_flags = new byte[2],
            add_submission = new NativeSubmissionInfo { reserved = new byte[3] },
            cancel_submission = new NativeSubmissionInfo { reserved = new byte[3] },
            recovery_submission = new NativeSubmissionInfo { reserved = new byte[3] },
            journal_path = new byte[PathCapacity],
            message = new byte[MessageCapacity]
        };
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NativeReconciliationResult
    {
        public uint struct_size;
        public ushort abi_version;
        public ushort reserved0;
        public byte ok;
        public byte run_found;
        public byte resolved;
        public byte locks_retained;
        public uint lifecycle_state;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = PathCapacity)]
        public byte[]? journal_path;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = MessageCapacity)]
        public byte[]? message;

        public static NativeReconciliationResult Create() => new()
        {
            struct_size = (uint)Marshal.SizeOf<NativeReconciliationResult>(),
            abi_version = AbiVersion,
            journal_path = new byte[PathCapacity],
            message = new byte[MessageCapacity]
        };
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate uint AbiVersionFn();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate uint SizeofFn();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate uint AlignofFn();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate MoexResult CreateHostFn(ref NativeCreateParams request, out IntPtr handle);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate MoexResult DestroyHostFn(IntPtr handle);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate MoexResult HandleFn(IntPtr handle);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate MoexResult GetSnapshotFn(IntPtr handle, ref NativeSnapshot snapshot);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate MoexResult GetPlanInfoFn(IntPtr handle, ref NativePlanInfo plan);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate MoexResult CopyPlanCanonicalFn(IntPtr handle, IntPtr buffer, uint capacity, out uint written);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate MoexResult AuthorizeFn(IntPtr handle, IntPtr canonical, uint canonicalSize, IntPtr sha256);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate MoexResult RunOrderTestFn(IntPtr handle, ref NativeOrderTestResult result);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate MoexResult ReconcileFn(IntPtr handle, ref NativeReconciliationResult result);
}
