#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOEX_C_ABI_VERSION 1u
#define MOEX_SYMBOL_CAPACITY 32u
#define MOEX_BOARD_CAPACITY 16u
#define MOEX_EXCHANGE_CAPACITY 16u
#define MOEX_GROUP_CAPACITY 32u
#define MOEX_PORTFOLIO_CAPACITY 32u
#define MOEX_ACCOUNT_CAPACITY 32u
#define MOEX_ORDER_ID_CAPACITY 40u
#define MOEX_INFO_CAPACITY 128u

typedef struct MoexHandleTag* MoexConnectorHandle;

typedef enum MoexResult {
    MOEX_RESULT_OK = 0,
    MOEX_RESULT_INVALID_ARGUMENT = 1,
    MOEX_RESULT_NOT_SUPPORTED = 2,
    MOEX_RESULT_NOT_INITIALIZED = 3,
    MOEX_RESULT_ALREADY_STARTED = 4,
    MOEX_RESULT_NOT_STARTED = 5,
    MOEX_RESULT_OVERFLOW = 6,
    MOEX_RESULT_INTERNAL_ERROR = 255
} MoexResult;

typedef enum MoexEventType {
    MOEX_EVENT_UNSPECIFIED = 0,
    MOEX_EVENT_CONNECTOR_STATUS = 1,
    MOEX_EVENT_ORDER_STATUS = 2,
    MOEX_EVENT_PRIVATE_TRADE = 3,
    MOEX_EVENT_POSITION = 4,
    MOEX_EVENT_PUBLIC_L1 = 5,
    MOEX_EVENT_PUBLIC_DIAGNOSTIC = 6,
    MOEX_EVENT_FULL_ORDER_LOG = 7,
    MOEX_EVENT_CERT_STEP = 8,
    MOEX_EVENT_ORDER_BOOK = 9,
    MOEX_EVENT_INSTRUMENT = 10,
    MOEX_EVENT_REPLAY_STATE = 11,
    MOEX_EVENT_PUBLIC_TRADE = 12
} MoexEventType;

typedef enum MoexReplayState {
    MOEX_REPLAY_STATE_UNSPECIFIED = 0,
    MOEX_REPLAY_STATE_STARTED = 1,
    MOEX_REPLAY_STATE_DRAINED = 2
} MoexReplayState;

typedef enum MoexNativeOrderStatus {
    MOEX_NATIVE_ORDER_STATUS_UNSPECIFIED = 0,
    MOEX_NATIVE_ORDER_STATUS_NEW = 1,
    MOEX_NATIVE_ORDER_STATUS_PARTIAL_FILL = 2,
    MOEX_NATIVE_ORDER_STATUS_FILLED = 3,
    MOEX_NATIVE_ORDER_STATUS_CANCELED = 4,
    MOEX_NATIVE_ORDER_STATUS_REJECTED = 5
} MoexNativeOrderStatus;

typedef enum MoexSourceConnector {
    MOEX_SOURCE_UNKNOWN = 0,
    MOEX_SOURCE_TWIME_TRADE = 1,
    MOEX_SOURCE_FIX_TRADE = 2,
    MOEX_SOURCE_FAST_MD = 3,
    MOEX_SOURCE_SIMBA_MD = 4,
    MOEX_SOURCE_PLAZA2_REPL = 5,
    MOEX_SOURCE_PLAZA2_TRADE = 6
} MoexSourceConnector;

typedef struct MoexEventHeader {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t event_type;
    uint64_t connector_seq;
    uint64_t source_seq;
    int64_t monotonic_time_ns;
    int64_t exchange_time_utc_ns;
    int64_t source_time_utc_ns;
    int64_t socket_receive_monotonic_ns;
    int64_t decode_monotonic_ns;
    int64_t publish_monotonic_ns;
    int64_t managed_poll_monotonic_ns;
    uint32_t source_connector;
    uint32_t flags;
} MoexEventHeader;

typedef struct MoexBackpressureCounters {
    uint64_t produced;
    uint64_t polled;
    uint64_t dropped;
    uint64_t high_watermark;
    bool overflowed;
    uint8_t reserved[7];
} MoexBackpressureCounters;

typedef struct MoexHealthSnapshot {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    uint32_t connector_state;
    uint32_t active_profile_kind;
    uint8_t prod_armed;
    uint8_t shadow_mode_enabled;
    uint8_t reserved1[6];
} MoexHealthSnapshot;

typedef struct MoexConnectorCreateParams {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    const char* connector_name;
    const char* instance_id;
} MoexConnectorCreateParams;

typedef struct MoexProfileLoadParams {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    const char* profile_path;
    uint8_t armed;
    uint8_t reserved1[7];
} MoexProfileLoadParams;

typedef struct MoexOrderSubmitRequest {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    const char* profile_id;
    const char* symbol;
    const char* account;
    const char* client_order_id;
} MoexOrderSubmitRequest;

typedef struct MoexOrderCancelRequest {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    const char* profile_id;
    const char* account;
    const char* server_order_id;
    const char* client_order_id;
} MoexOrderCancelRequest;

typedef struct MoexOrderReplaceRequest {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    const char* profile_id;
    const char* account;
    const char* server_order_id;
    const char* client_order_id;
} MoexOrderReplaceRequest;

typedef struct MoexMassCancelRequest {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    const char* profile_id;
    const char* account;
    const char* instrument_scope;
} MoexMassCancelRequest;

typedef struct MoexSubscriptionRequest {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    const char* profile_id;
    const char* stream_name;
    const char* symbol;
    const char* board;
} MoexSubscriptionRequest;

typedef struct MoexPolledEvent {
    MoexEventHeader header;
    uint32_t payload_size;
    uint16_t payload_version;
    uint16_t replay_state;
    int32_t status;
    int32_t side;
    int32_t level;
    int32_t update_type;
    double price;
    double quantity;
    double secondary_price;
    double secondary_quantity;
    double cumulative_quantity;
    double remaining_quantity;
    double average_price;
    double open_profit_loss;
    uint8_t prefer_order_book_l1;
    uint8_t existing;
    uint8_t read_only_shadow;
    uint8_t reserved0;
    char symbol[MOEX_SYMBOL_CAPACITY];
    char board[MOEX_BOARD_CAPACITY];
    char exchange[MOEX_EXCHANGE_CAPACITY];
    char instrument_group[MOEX_GROUP_CAPACITY];
    char portfolio[MOEX_PORTFOLIO_CAPACITY];
    char trade_account[MOEX_ACCOUNT_CAPACITY];
    char server_order_id[MOEX_ORDER_ID_CAPACITY];
    char client_order_id[MOEX_ORDER_ID_CAPACITY];
    char info_text[MOEX_INFO_CAPACITY];
} MoexPolledEvent;

typedef void (*MoexLowRateCallback)(const MoexEventHeader* header, const void* payload, void* user_data);

const char* moex_phase0_abi_name(void);
uint32_t moex_phase0_abi_version(void);
bool moex_phase0_prod_requires_arm(const char* environment, bool armed);
uint32_t moex_sizeof_event_header(void);
uint32_t moex_sizeof_backpressure_counters(void);
uint32_t moex_sizeof_health_snapshot(void);
uint32_t moex_sizeof_connector_create_params(void);
uint32_t moex_sizeof_polled_event(void);
MoexResult moex_create_connector(const MoexConnectorCreateParams* params, MoexConnectorHandle* out_handle);
MoexResult moex_destroy_connector(MoexConnectorHandle handle);
MoexResult moex_load_profile(MoexConnectorHandle handle, const MoexProfileLoadParams* params);
MoexResult moex_load_synthetic_replay(MoexConnectorHandle handle, const char* replay_path);
MoexResult moex_start_connector(MoexConnectorHandle handle);
MoexResult moex_stop_connector(MoexConnectorHandle handle);
MoexResult moex_submit_order_placeholder(MoexConnectorHandle handle, const MoexOrderSubmitRequest* request);
MoexResult moex_cancel_order_placeholder(MoexConnectorHandle handle, const MoexOrderCancelRequest* request);
MoexResult moex_replace_order_placeholder(MoexConnectorHandle handle, const MoexOrderReplaceRequest* request);
MoexResult moex_mass_cancel_placeholder(MoexConnectorHandle handle, const MoexMassCancelRequest* request);
MoexResult moex_subscribe_placeholder(MoexConnectorHandle handle, const MoexSubscriptionRequest* request);
MoexResult moex_unsubscribe_placeholder(MoexConnectorHandle handle, const MoexSubscriptionRequest* request);
MoexResult moex_poll_events(MoexConnectorHandle handle, void* out_events, uint32_t capacity, uint32_t* written);
MoexResult moex_register_low_rate_callback(MoexConnectorHandle handle, MoexLowRateCallback callback, void* user_data);
MoexResult moex_get_health(MoexConnectorHandle handle, MoexHealthSnapshot* out_health);
MoexResult moex_get_backpressure_counters(MoexConnectorHandle handle, MoexBackpressureCounters* out_counters);
MoexResult moex_flush_recovery_state(MoexConnectorHandle handle);

#ifdef __cplusplus
}
#endif
