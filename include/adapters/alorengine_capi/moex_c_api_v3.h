#pragma once

#include "adapters/alorengine_capi/moex_c_api.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOEX_C_ABI_V3_VERSION 3u
#define MOEX_V3_TEXT_CAPACITY 128u
#define MOEX_V3_PATH_CAPACITY 512u
#define MOEX_V3_SHA256_CAPACITY 65u
#define MOEX_V3_SYMBOL_CAPACITY 64u
#define MOEX_V3_MESSAGE_CAPACITY 256u
#define MOEX_V3_MAX_STREAMS 16u

typedef struct MoexConnectorHostV3Tag* MoexConnectorHostHandleV3;

typedef enum MoexConnectorHostStateV3 {
    MOEX_V3_HOST_CREATED = 0,
    MOEX_V3_HOST_STARTED = 1,
    MOEX_V3_HOST_READY = 2,
    MOEX_V3_HOST_STOPPING = 3,
    MOEX_V3_HOST_STOPPED = 4,
    MOEX_V3_HOST_FAILED = 5
} MoexConnectorHostStateV3;

typedef enum MoexPersistentOrderStateV3 {
    MOEX_V3_ORDER_DEFINITELY_NOT_SENT = 0,
    MOEX_V3_ORDER_POSSIBLY_SENT = 1,
    MOEX_V3_ORDER_POSTED = 2,
    MOEX_V3_ORDER_REJECTED = 3,
    MOEX_V3_ORDER_WORKING = 4,
    MOEX_V3_ORDER_PARTIALLY_FILLED = 5,
    MOEX_V3_ORDER_FILLED = 6,
    MOEX_V3_ORDER_CANCEL_PENDING = 7,
    MOEX_V3_ORDER_CANCELLED = 8,
    MOEX_V3_ORDER_UNRESOLVED_ORPHAN_INCIDENT = 9,
    MOEX_V3_ORDER_IDLE = 10,
    MOEX_V3_ORDER_AUTHORIZED = 11,
    MOEX_V3_ORDER_ADD_PENDING = 12
} MoexPersistentOrderStateV3;

typedef enum MoexPositionEvidenceClassV3 {
    MOEX_V3_POSITION_EXACT_ZERO_POS_ROW = 0,
    MOEX_V3_POSITION_FLAT_BY_POS_SNAPSHOT_AND_TRADE_REPLAY = 1,
    MOEX_V3_POSITION_UNRESOLVED = 2
} MoexPositionEvidenceClassV3;

typedef enum MoexSubmissionCertaintyV3 {
    MOEX_V3_SUBMISSION_DEFINITELY_NOT_SENT = 0,
    MOEX_V3_SUBMISSION_POSSIBLY_SENT = 1,
    MOEX_V3_SUBMISSION_POSTED = 2
} MoexSubmissionCertaintyV3;

/* Host configuration contains only session/account/static safety state.  The
 * actual order terms are supplied by MoexPersistentOrderRequestV3 per epoch. */
typedef struct MoexConnectorHostCreateParamsV3 {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
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
    int32_t base_ext_id;
    uint32_t base_add_user_id;
    uint32_t base_cancel_user_id;
    uint32_t base_recovery_user_id;
    const char* base_run_id;
    const char* journal_root;
    const char* receipt_path;
    const char* profile_id;
    const char* profile_fingerprint;
    const char* policy_version;
    const char* policy_sha256;
    uint8_t reserved[32];
} MoexConnectorHostCreateParamsV3;

typedef struct MoexPersistentOrderRequestV3 {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    uint32_t side;
    const char* price;
    const char* base_contract_code;
    const char* comment;
    int32_t quantity;
    uint8_t reserved[32];
} MoexPersistentOrderRequestV3;

typedef struct MoexV3StreamHealth {
    uint32_t stream_code;
    uint8_t online;
    uint8_t snapshot_complete;
    uint8_t periodic_snapshot_consistent;
    uint8_t has_publication_state;
    uint64_t committed_row_count;
    uint64_t last_commit_sequence;
    int32_t publication_state;
    int32_t reserved0;
} MoexV3StreamHealth;

typedef struct MoexV3TargetProvenance {
    uint32_t stream_code;
    uint32_t table_code;
    int64_t repl_rev;
    uint64_t lifenum;
    uint8_t present;
    uint8_t reserved[7];
    int64_t typed_row_key;
} MoexV3TargetProvenance;

typedef struct MoexV3ReplyInfo {
    uint8_t present;
    uint8_t accepted;
    uint8_t timed_out;
    uint8_t order_id_present;
    int32_t code;
    uint32_t reserved0;
    int64_t order_id;
} MoexV3ReplyInfo;

typedef struct MoexV3SubmissionInfo {
    uint32_t certainty;
    uint8_t post_invoked;
    uint8_t reserved[3];
} MoexV3SubmissionInfo;

typedef struct MoexPersistentSnapshotV3 {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    uint32_t host_state;
    uint32_t purpose;
    uint32_t environment;
    uint32_t transport_mode;
    char runtime_compatibility[MOEX_V3_TEXT_CAPACITY];
    char runtime_scheme_sha256[MOEX_V3_SHA256_CAPACITY];
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
    char target[MOEX_V3_SYMBOL_CAPACITY];
    char min_step[MOEX_V3_TEXT_CAPACITY];
    uint8_t session_status_present;
    uint8_t instrument_status_present;
    uint8_t trade_anchor_present;
    uint8_t lifecycle_state_present;
    int32_t session_status;
    int32_t instrument_status;
    char bid[MOEX_V3_TEXT_CAPACITY];
    char ask[MOEX_V3_TEXT_CAPACITY];
    int64_t bbo_age_ms;
    uint64_t refdata_lifenum;
    MoexV3TargetProvenance fut_instruments_provenance;
    MoexV3TargetProvenance fut_sess_contents_provenance;
    MoexV3TargetProvenance session_provenance;
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
    uint8_t order_epoch_active;
    uint8_t order_authorized;
    uint8_t order_submission_attempted;
    uint8_t new_order_allowed;
    uint32_t lifecycle_state;
    uint32_t reserved3;
    int64_t order_id;
    int64_t original_quantity;
    int64_t remaining_quantity;
    int64_t executed_quantity;
    uint8_t market_safe;
    uint8_t evidence_consistent;
    uint8_t reserved_flags[6];
    MoexV3ReplyInfo add_reply;
    MoexV3ReplyInfo cancel_reply;
    uint64_t cg_pub_msgnew;
    uint64_t cg_pub_post;
    uint32_t stream_count;
    uint32_t reserved4;
    MoexV3StreamHealth streams[MOEX_V3_MAX_STREAMS];
    char last_error[MOEX_V3_MESSAGE_CAPACITY];
} MoexPersistentSnapshotV3;

typedef struct MoexPersistentPlanInfoV3 {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    uint8_t ok;
    uint8_t reserved_flags[3];
    uint32_t failure;
    uint32_t side;
    int64_t quantity;
    char price[MOEX_V3_TEXT_CAPACITY];
    char plan_sha256[MOEX_V3_SHA256_CAPACITY];
    char add_payload_sha256[MOEX_V3_SHA256_CAPACITY];
    char recovery_payload_sha256[MOEX_V3_SHA256_CAPACITY];
    uint32_t canonical_size;
    uint32_t reserved1;
    char message[MOEX_V3_MESSAGE_CAPACITY];
} MoexPersistentPlanInfoV3;

typedef struct MoexPersistentOrderResultV3 {
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
    MoexV3SubmissionInfo add_submission;
    MoexV3SubmissionInfo cancel_submission;
    MoexV3SubmissionInfo recovery_submission;
    MoexV3ReplyInfo add_reply;
    MoexV3ReplyInfo cancel_reply;
    MoexV3ReplyInfo recovery_reply;
    char journal_path[MOEX_V3_PATH_CAPACITY];
    char message[MOEX_V3_MESSAGE_CAPACITY];
} MoexPersistentOrderResultV3;

typedef struct MoexPersistentReconciliationResultV3 {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    uint8_t ok;
    uint8_t run_found;
    uint8_t resolved;
    uint8_t locks_retained;
    uint32_t lifecycle_state;
    char journal_path[MOEX_V3_PATH_CAPACITY];
    char message[MOEX_V3_MESSAGE_CAPACITY];
} MoexPersistentReconciliationResultV3;

uint32_t moex_v3_abi_version(void);
MoexResult moex_v3_create_host(const MoexConnectorHostCreateParamsV3* params, MoexConnectorHostHandleV3* out_handle);
MoexResult moex_v3_destroy_host(MoexConnectorHostHandleV3 handle);
MoexResult moex_v3_start(MoexConnectorHostHandleV3 handle);
MoexResult moex_v3_poll(MoexConnectorHostHandleV3 handle);
MoexResult moex_v3_stop(MoexConnectorHostHandleV3 handle);
MoexResult moex_v3_get_snapshot(MoexConnectorHostHandleV3 handle, MoexPersistentSnapshotV3* out_snapshot);
MoexResult moex_v3_plan_order(MoexConnectorHostHandleV3 handle, const MoexPersistentOrderRequestV3* request,
                              MoexPersistentPlanInfoV3* out_plan);
MoexResult moex_v3_copy_plan_canonical(MoexConnectorHostHandleV3 handle, void* buffer, uint32_t capacity,
                                       uint32_t* written);
MoexResult moex_v3_begin_order(MoexConnectorHostHandleV3 handle, const MoexPersistentOrderRequestV3* request,
                               const void* canonical_bytes, uint32_t canonical_size, const char* full_sha256);
MoexResult moex_v3_submit_order(MoexConnectorHostHandleV3 handle, MoexPersistentOrderResultV3* out_result);
MoexResult moex_v3_poll_order(MoexConnectorHostHandleV3 handle, MoexPersistentOrderResultV3* out_result);
MoexResult moex_v3_cancel_current_order(MoexConnectorHostHandleV3 handle, MoexPersistentOrderResultV3* out_result);
MoexResult moex_v3_finish_order_epoch(MoexConnectorHostHandleV3 handle);
MoexResult moex_v3_reconcile(MoexConnectorHostHandleV3 handle, MoexPersistentReconciliationResultV3* out_result);

#define MOEX_V3_LAYOUT_EXPORTS(name)                                                                                   \
    uint32_t moex_v3_sizeof_##name(void);                                                                              \
    uint32_t moex_v3_alignof_##name(void)

MOEX_V3_LAYOUT_EXPORTS(MoexConnectorHostCreateParamsV3);
MOEX_V3_LAYOUT_EXPORTS(MoexPersistentOrderRequestV3);
MOEX_V3_LAYOUT_EXPORTS(MoexV3StreamHealth);
MOEX_V3_LAYOUT_EXPORTS(MoexV3TargetProvenance);
MOEX_V3_LAYOUT_EXPORTS(MoexV3ReplyInfo);
MOEX_V3_LAYOUT_EXPORTS(MoexV3SubmissionInfo);
MOEX_V3_LAYOUT_EXPORTS(MoexPersistentSnapshotV3);
MOEX_V3_LAYOUT_EXPORTS(MoexPersistentPlanInfoV3);
MOEX_V3_LAYOUT_EXPORTS(MoexPersistentOrderResultV3);
MOEX_V3_LAYOUT_EXPORTS(MoexPersistentReconciliationResultV3);

#undef MOEX_V3_LAYOUT_EXPORTS

#ifdef __cplusplus
}
#endif
