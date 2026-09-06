#pragma once

#include "adapters/alorengine_capi/moex_c_api.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOEX_C_ABI_V2_VERSION 2u
#define MOEX_V2_TEXT_CAPACITY 128u
#define MOEX_V2_PATH_CAPACITY 512u
#define MOEX_V2_SHA256_CAPACITY 65u
#define MOEX_V2_SYMBOL_CAPACITY 64u
#define MOEX_V2_MESSAGE_CAPACITY 256u
#define MOEX_V2_MAX_STREAMS 16u

typedef struct MoexConnectorHostV2Tag* MoexConnectorHostHandleV2;

typedef enum MoexConnectorHostPurposeV2 {
    MOEX_V2_PURPOSE_QUALIFY = 0,
    MOEX_V2_PURPOSE_ORDER_TEST = 1
} MoexConnectorHostPurposeV2;

typedef enum MoexConnectorHostStateV2 {
    MOEX_V2_HOST_CREATED = 0,
    MOEX_V2_HOST_STARTED = 1,
    MOEX_V2_HOST_READY = 2,
    MOEX_V2_HOST_STOPPING = 3,
    MOEX_V2_HOST_STOPPED = 4,
    MOEX_V2_HOST_FAILED = 5
} MoexConnectorHostStateV2;

typedef enum MoexPositionEvidenceClassV2 {
    MOEX_V2_POSITION_EXACT_ZERO_POS_ROW = 0,
    MOEX_V2_POSITION_FLAT_BY_POS_SNAPSHOT_AND_TRADE_REPLAY = 1,
    MOEX_V2_POSITION_UNRESOLVED = 2
} MoexPositionEvidenceClassV2;

typedef enum MoexSubmissionCertaintyV2 {
    MOEX_V2_SUBMISSION_DEFINITELY_NOT_SENT = 0,
    MOEX_V2_SUBMISSION_POSSIBLY_SENT = 1,
    MOEX_V2_SUBMISSION_POSTED = 2
} MoexSubmissionCertaintyV2;

typedef struct MoexConnectorHostCreateParamsV2 {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t purpose;
    uint32_t reserved0;
    const char* runtime_root;
    const char* library_path;
    const char* scheme_dir;
    const char* config_dir;
    const char* env_settings_env_var;
    const char* credentials_env_var;
    const char* software_key_env_var;
    const char* broker_code_env_var;
    const char* client_code_env_var;
    const char* expected_release;
    const char* expected_scheme_sha256;
    int64_t isin_id;
    int32_t session_id;
    uint32_t reserved1;
    uint8_t armed_test_network;
    uint8_t armed_test_session;
    uint8_t armed_test_plaza2;
    uint8_t armed_test_order_send;
    uint8_t reserved_flags[4];
    uint32_t side;
    const char* price;
    const char* base_contract_code;
    const char* comment;
    int32_t ext_id;
    uint32_t add_user_id;
    uint32_t cancel_user_id;
    uint32_t recovery_user_id;
    const char* run_id;
    const char* journal_root;
    const char* receipt_path;
    const char* profile_id;
    const char* profile_fingerprint;
    const char* policy_version;
    const char* policy_sha256;
    uint8_t reserved[32];
} MoexConnectorHostCreateParamsV2;

typedef struct MoexV2StreamHealth {
    uint32_t stream_code;
    uint8_t online;
    uint8_t snapshot_complete;
    uint8_t periodic_snapshot_consistent;
    uint8_t has_publication_state;
    uint64_t committed_row_count;
    uint64_t last_commit_sequence;
    int32_t publication_state;
    int32_t reserved0;
} MoexV2StreamHealth;

typedef struct MoexV2TargetProvenance {
    uint32_t stream_code;
    uint32_t table_code;
    int64_t repl_rev;
    uint64_t lifenum;
    uint8_t present;
    uint8_t reserved[7];
    int64_t typed_row_key;
} MoexV2TargetProvenance;

typedef struct MoexConnectorHostSnapshotV2 {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    uint32_t host_state;
    uint32_t purpose;
    uint32_t environment;
    uint32_t transport_mode;
    char runtime_compatibility[MOEX_V2_TEXT_CAPACITY];
    char runtime_scheme_sha256[MOEX_V2_SHA256_CAPACITY];
    uint8_t publisher_ready;
    uint8_t reply_ready;
    uint8_t private_streams_ready;
    uint8_t observation_ready;
    uint8_t target_refdata_provenance_ready;
    uint8_t target_aggr20_uncrossed;
    uint8_t uob_periodic_consistent;
    uint8_t zero_starting_position_proven;
    int64_t target_isin_id;
    int32_t session_id;
    uint32_t reserved1;
    char target[MOEX_V2_SYMBOL_CAPACITY];
    char min_step[MOEX_V2_TEXT_CAPACITY];
    uint8_t session_status_present;
    uint8_t instrument_status_present;
    uint8_t trade_anchor_present;
    uint8_t lifecycle_state_present;
    int32_t session_status;
    int32_t instrument_status;
    char bid[MOEX_V2_TEXT_CAPACITY];
    char ask[MOEX_V2_TEXT_CAPACITY];
    int64_t bbo_age_ms;
    uint64_t refdata_lifenum;
    MoexV2TargetProvenance fut_instruments_provenance;
    MoexV2TargetProvenance fut_sess_contents_provenance;
    MoexV2TargetProvenance session_provenance;
    uint32_t position_evidence_class;
    int64_t pos_trades_rev;
    int64_t pos_trades_lifenum;
    int64_t trade_anchor_trades_rev;
    int64_t trade_anchor_trades_lifenum;
    int64_t trade_anchor_server_time;
    uint8_t trade_replay_complete;
    uint8_t limits_set;
    uint16_t reserved2;
    uint64_t active_own_order_count;
    uint32_t lifecycle_state;
    uint32_t reserved3;
    int64_t order_id;
    int64_t original_quantity;
    int64_t remaining_quantity;
    int64_t executed_quantity;
    uint8_t market_safe;
    uint8_t evidence_consistent;
    uint8_t reserved_flags[6];
    uint64_t cg_pub_msgnew;
    uint64_t cg_pub_post;
    uint32_t stream_count;
    uint32_t reserved4;
    MoexV2StreamHealth streams[MOEX_V2_MAX_STREAMS];
    char last_error[MOEX_V2_MESSAGE_CAPACITY];
} MoexConnectorHostSnapshotV2;

typedef struct MoexPreSendPlanInfoV2 {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    uint8_t ok;
    uint8_t reserved_flags[3];
    uint32_t failure;
    uint32_t side;
    int64_t quantity;
    char price[MOEX_V2_TEXT_CAPACITY];
    char plan_sha256[MOEX_V2_SHA256_CAPACITY];
    char add_payload_sha256[MOEX_V2_SHA256_CAPACITY];
    char recovery_payload_sha256[MOEX_V2_SHA256_CAPACITY];
    uint32_t canonical_size;
    uint32_t reserved1;
    char message[MOEX_V2_MESSAGE_CAPACITY];
} MoexPreSendPlanInfoV2;

typedef struct MoexV2ReplyInfo {
    uint8_t present;
    uint8_t accepted;
    uint8_t timed_out;
    uint8_t order_id_present;
    int32_t code;
    uint32_t reserved0;
    int64_t order_id;
} MoexV2ReplyInfo;

typedef struct MoexV2SubmissionInfo {
    uint32_t certainty;
    uint8_t post_invoked;
    uint8_t reserved[3];
} MoexV2SubmissionInfo;

typedef struct MoexOrderTestResultV2 {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    uint8_t ok;
    uint8_t market_safe_terminal;
    uint8_t journal_ok;
    uint8_t journal_degraded;
    uint8_t evidence_consistent;
    uint8_t orphan_incident_written;
    uint8_t reserved_flags[2];
    uint32_t lifecycle_state;
    int64_t order_id;
    int64_t original_quantity;
    int64_t remaining_quantity;
    int64_t executed_quantity;
    MoexV2SubmissionInfo add_submission;
    MoexV2SubmissionInfo cancel_submission;
    MoexV2SubmissionInfo recovery_submission;
    MoexV2ReplyInfo add_reply;
    MoexV2ReplyInfo cancel_reply;
    MoexV2ReplyInfo recovery_reply;
    char journal_path[MOEX_V2_PATH_CAPACITY];
    char message[MOEX_V2_MESSAGE_CAPACITY];
} MoexOrderTestResultV2;

typedef struct MoexRestartReconciliationResultV2 {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    uint8_t ok;
    uint8_t run_found;
    uint8_t resolved;
    uint8_t locks_retained;
    uint32_t lifecycle_state;
    char journal_path[MOEX_V2_PATH_CAPACITY];
    char message[MOEX_V2_MESSAGE_CAPACITY];
} MoexRestartReconciliationResultV2;

uint32_t moex_v2_abi_version(void);

MoexResult moex_v2_create_host(const MoexConnectorHostCreateParamsV2* params, MoexConnectorHostHandleV2* out_handle);
MoexResult moex_v2_destroy_host(MoexConnectorHostHandleV2 handle);
MoexResult moex_v2_start(MoexConnectorHostHandleV2 handle);
MoexResult moex_v2_poll(MoexConnectorHostHandleV2 handle);
MoexResult moex_v2_stop(MoexConnectorHostHandleV2 handle);
MoexResult moex_v2_get_snapshot(MoexConnectorHostHandleV2 handle, MoexConnectorHostSnapshotV2* out_snapshot);
MoexResult moex_v2_get_plan_info(MoexConnectorHostHandleV2 handle, MoexPreSendPlanInfoV2* out_plan);
MoexResult moex_v2_copy_plan_canonical(MoexConnectorHostHandleV2 handle, void* buffer, uint32_t capacity,
                                       uint32_t* written);
MoexResult moex_v2_authorize(MoexConnectorHostHandleV2 handle, const void* canonical_bytes, uint32_t canonical_size,
                             const char* full_sha256);
MoexResult moex_v2_run_order_test(MoexConnectorHostHandleV2 handle, MoexOrderTestResultV2* out_result);
MoexResult moex_v2_reconcile(MoexConnectorHostHandleV2 handle, MoexRestartReconciliationResultV2* out_result);

#define MOEX_V2_LAYOUT_EXPORTS(name)                                                                                   \
    uint32_t moex_v2_sizeof_##name(void);                                                                              \
    uint32_t moex_v2_alignof_##name(void)

MOEX_V2_LAYOUT_EXPORTS(MoexConnectorHostCreateParamsV2);
MOEX_V2_LAYOUT_EXPORTS(MoexV2StreamHealth);
MOEX_V2_LAYOUT_EXPORTS(MoexV2TargetProvenance);
MOEX_V2_LAYOUT_EXPORTS(MoexConnectorHostSnapshotV2);
MOEX_V2_LAYOUT_EXPORTS(MoexPreSendPlanInfoV2);
MOEX_V2_LAYOUT_EXPORTS(MoexV2ReplyInfo);
MOEX_V2_LAYOUT_EXPORTS(MoexV2SubmissionInfo);
MOEX_V2_LAYOUT_EXPORTS(MoexOrderTestResultV2);
MOEX_V2_LAYOUT_EXPORTS(MoexRestartReconciliationResultV2);

#undef MOEX_V2_LAYOUT_EXPORTS

#ifdef __cplusplus
}
#endif
