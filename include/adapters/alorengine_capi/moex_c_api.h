#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define MOEX_ABI_DEPRECATED __attribute__((deprecated))
#elif defined(_MSC_VER)
#define MOEX_ABI_DEPRECATED __declspec(deprecated)
#else
#define MOEX_ABI_DEPRECATED
#endif

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
#define MOEX_REPLSTATE_CAPACITY 160u
#define MOEX_NAME_CAPACITY 96u
#define MOEX_TEXT_CAPACITY 128u
#define MOEX_PRICE_TEXT_CAPACITY 32u
#define MOEX_REASON_CAPACITY 96u
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
    MOEX_RESULT_NULL_POINTER = 7,
    MOEX_RESULT_BUFFER_TOO_SMALL = 8,
    MOEX_RESULT_SNAPSHOT_UNAVAILABLE = 9,
    MOEX_RESULT_UNAVAILABLE = 9,
    MOEX_RESULT_TRANSLATION_FAILED = 10,
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

typedef enum MoexPlaza2InstrumentKind {
    MOEX_PLAZA2_INSTRUMENT_KIND_UNKNOWN = 0,
    MOEX_PLAZA2_INSTRUMENT_KIND_FUTURE = 1,
    MOEX_PLAZA2_INSTRUMENT_KIND_OPTION = 2,
    MOEX_PLAZA2_INSTRUMENT_KIND_MULTILEG = 3
} MoexPlaza2InstrumentKind;

typedef enum MoexPlaza2PositionScope {
    MOEX_PLAZA2_POSITION_SCOPE_CLIENT = 0,
    MOEX_PLAZA2_POSITION_SCOPE_SETTLEMENT_ACCOUNT = 1
} MoexPlaza2PositionScope;

typedef enum MoexPlaza2Side {
    MOEX_PLAZA2_SIDE_UNKNOWN = 0,
    MOEX_PLAZA2_SIDE_BUY = 1,
    MOEX_PLAZA2_SIDE_SELL = 2
} MoexPlaza2Side;

typedef enum MoexPlaza2ReconciliationSource {
    MOEX_PLAZA2_RECONCILIATION_SOURCE_UNKNOWN = 0,
    MOEX_PLAZA2_RECONCILIATION_SOURCE_TWIME = 1,
    MOEX_PLAZA2_RECONCILIATION_SOURCE_PLAZA = 2
} MoexPlaza2ReconciliationSource;

typedef enum MoexPlaza2MatchMode {
    MOEX_PLAZA2_MATCH_MODE_NONE = 0,
    MOEX_PLAZA2_MATCH_MODE_DIRECT_IDENTIFIER = 1,
    MOEX_PLAZA2_MATCH_MODE_EXACT_FALLBACK_TUPLE = 2,
    MOEX_PLAZA2_MATCH_MODE_AMBIGUOUS_CANDIDATES = 3
} MoexPlaza2MatchMode;

typedef enum MoexPlaza2OrderStatus {
    MOEX_PLAZA2_ORDER_STATUS_PROVISIONAL_TWIME = 0,
    MOEX_PLAZA2_ORDER_STATUS_CONFIRMED = 1,
    MOEX_PLAZA2_ORDER_STATUS_REJECTED = 2,
    MOEX_PLAZA2_ORDER_STATUS_CANCELED = 3,
    MOEX_PLAZA2_ORDER_STATUS_FILLED = 4,
    MOEX_PLAZA2_ORDER_STATUS_AMBIGUOUS = 5,
    MOEX_PLAZA2_ORDER_STATUS_DIVERGED = 6,
    MOEX_PLAZA2_ORDER_STATUS_STALE = 7,
    MOEX_PLAZA2_ORDER_STATUS_UNKNOWN = 8
} MoexPlaza2OrderStatus;

typedef enum MoexPlaza2TradeStatus {
    MOEX_PLAZA2_TRADE_STATUS_TWIME_ONLY = 0,
    MOEX_PLAZA2_TRADE_STATUS_PLAZA_ONLY = 1,
    MOEX_PLAZA2_TRADE_STATUS_MATCHED = 2,
    MOEX_PLAZA2_TRADE_STATUS_DIVERGED = 3,
    MOEX_PLAZA2_TRADE_STATUS_AMBIGUOUS = 4
} MoexPlaza2TradeStatus;

typedef enum MoexPlaza2TwimeOrderKind {
    MOEX_PLAZA2_TWIME_ORDER_KIND_NEW_INTENT = 0,
    MOEX_PLAZA2_TWIME_ORDER_KIND_NEW_ACCEPTED = 1,
    MOEX_PLAZA2_TWIME_ORDER_KIND_REPLACE_INTENT = 2,
    MOEX_PLAZA2_TWIME_ORDER_KIND_REPLACE_ACCEPTED = 3,
    MOEX_PLAZA2_TWIME_ORDER_KIND_CANCEL_INTENT = 4,
    MOEX_PLAZA2_TWIME_ORDER_KIND_CANCEL_ACCEPTED = 5,
    MOEX_PLAZA2_TWIME_ORDER_KIND_REJECTED = 6
} MoexPlaza2TwimeOrderKind;

typedef enum MoexPlaza2TwimeTradeKind { MOEX_PLAZA2_TWIME_TRADE_KIND_EXECUTION = 0 } MoexPlaza2TwimeTradeKind;

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
    uint8_t overflowed;
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

typedef struct MoexPlaza2PrivateConnectorHealth {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    uint8_t open;
    uint8_t closed;
    uint8_t snapshot_active;
    uint8_t online;
    uint8_t transaction_open;
    uint8_t reserved1[3];
    uint64_t commit_count;
    uint64_t callback_error_count;
} MoexPlaza2PrivateConnectorHealth;

typedef struct MoexPlaza2ResumeMarkers {
    uint32_t struct_size;
    uint16_t abi_version;
    uint8_t has_lifenum;
    uint8_t reserved0;
    uint64_t last_lifenum;
    char last_replstate[MOEX_REPLSTATE_CAPACITY];
} MoexPlaza2ResumeMarkers;

typedef struct MoexPlaza2StreamHealthItem {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved2;
    uint32_t stream_code;
    uint8_t online;
    uint8_t snapshot_complete;
    uint8_t has_publication_state;
    uint8_t reserved0;
    int32_t publication_state;
    int32_t last_event_type;
    int32_t reserved1;
    uint64_t clear_deleted_count;
    uint64_t committed_row_count;
    uint64_t last_commit_sequence;
    int64_t last_trades_rev;
    int64_t last_trades_lifenum;
    int64_t last_server_time;
    int64_t last_info_moment;
    int64_t last_event_id;
    char stream_name[MOEX_GROUP_CAPACITY];
    char last_message[MOEX_INFO_CAPACITY];
} MoexPlaza2StreamHealthItem;

typedef struct MoexPlaza2TradingSessionItem {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved1;
    int32_t sess_id;
    int32_t state;
    int32_t inter_cl_state;
    uint8_t eve_on;
    uint8_t mon_on;
    uint8_t reserved0[2];
    int64_t begin;
    int64_t end;
    int64_t inter_cl_begin;
    int64_t inter_cl_end;
    int64_t eve_begin;
    int64_t eve_end;
    int64_t mon_begin;
    int64_t mon_end;
    int64_t settl_sess_begin;
    int64_t clr_sess_begin;
    int64_t settl_price_calc_time;
    int64_t settl_sess_t1_begin;
    int64_t margin_call_fix_schedule;
} MoexPlaza2TradingSessionItem;

typedef struct MoexPlaza2InstrumentItem {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved2;
    int32_t isin_id;
    int32_t sess_id;
    uint8_t kind;
    uint8_t put;
    uint8_t is_spread;
    uint8_t leg_count;
    int32_t fut_isin_id;
    int32_t option_series_id;
    int32_t inst_term;
    int32_t roundto;
    int32_t lot_volume;
    int32_t trade_mode_id;
    int32_t state;
    int32_t signs;
    int64_t last_trade_date;
    int64_t group_mask;
    int64_t trade_period_access;
    int32_t leg1_isin_id;
    int32_t leg1_qty_ratio;
    int8_t leg1_order_no;
    int8_t reserved0[3];
    int32_t leg2_isin_id;
    int32_t leg2_qty_ratio;
    int8_t leg2_order_no;
    int8_t reserved1[3];
    char isin[MOEX_SYMBOL_CAPACITY];
    char short_isin[MOEX_SYMBOL_CAPACITY];
    char name[MOEX_NAME_CAPACITY];
    char base_contract_code[MOEX_SYMBOL_CAPACITY];
    char min_step[MOEX_PRICE_TEXT_CAPACITY];
    char step_price[MOEX_PRICE_TEXT_CAPACITY];
    char settlement_price[MOEX_PRICE_TEXT_CAPACITY];
    char strike[MOEX_PRICE_TEXT_CAPACITY];
} MoexPlaza2InstrumentItem;

typedef struct MoexPlaza2MatchingMapItem {
    uint32_t struct_size;
    uint16_t abi_version;
    uint8_t reserved1[2];
    int32_t base_contract_id;
    int8_t matching_id;
    uint8_t reserved0[3];
} MoexPlaza2MatchingMapItem;

typedef struct MoexPlaza2LimitItem {
    uint32_t struct_size;
    uint16_t abi_version;
    uint8_t reserved1;
    uint8_t scope;
    uint8_t limits_set;
    uint8_t is_auto_update_limit;
    uint8_t reserved0;
    char account_code[MOEX_ACCOUNT_CAPACITY];
    char money_free[MOEX_PRICE_TEXT_CAPACITY];
    char money_blocked[MOEX_PRICE_TEXT_CAPACITY];
    char vm_reserve[MOEX_PRICE_TEXT_CAPACITY];
    char fee[MOEX_PRICE_TEXT_CAPACITY];
    char money_old[MOEX_PRICE_TEXT_CAPACITY];
    char money_amount[MOEX_PRICE_TEXT_CAPACITY];
    char money_pledge_amount[MOEX_PRICE_TEXT_CAPACITY];
    char actual_amount_of_base_currency[MOEX_PRICE_TEXT_CAPACITY];
    char vm_intercl[MOEX_PRICE_TEXT_CAPACITY];
    char broker_fee[MOEX_PRICE_TEXT_CAPACITY];
    char penalty[MOEX_PRICE_TEXT_CAPACITY];
    char premium_intercl[MOEX_PRICE_TEXT_CAPACITY];
    char net_option_value[MOEX_PRICE_TEXT_CAPACITY];
} MoexPlaza2LimitItem;

typedef struct MoexPlaza2PositionItem {
    uint32_t struct_size;
    uint16_t abi_version;
    uint8_t reserved1;
    uint8_t scope;
    int8_t account_type;
    uint8_t reserved0[2];
    int32_t isin_id;
    int64_t xpos;
    int64_t xbuys_qty;
    int64_t xsells_qty;
    int64_t xday_open_qty;
    int64_t xday_open_buys_qty;
    int64_t xday_open_sells_qty;
    int64_t xopen_qty;
    int64_t last_deal_id;
    int64_t last_quantity;
    char account_code[MOEX_ACCOUNT_CAPACITY];
    char waprice[MOEX_PRICE_TEXT_CAPACITY];
    char net_volume_rur[MOEX_PRICE_TEXT_CAPACITY];
} MoexPlaza2PositionItem;

typedef struct MoexPlaza2OwnOrderItem {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved2;
    uint8_t multileg;
    int8_t dir;
    int8_t public_action;
    int8_t private_action;
    int32_t sess_id;
    int32_t isin_id;
    int32_t ext_id;
    uint8_t from_trade_repl;
    uint8_t from_user_book;
    uint8_t from_current_day;
    uint8_t reserved0;
    int64_t public_order_id;
    int64_t private_order_id;
    int64_t public_amount;
    int64_t public_amount_rest;
    int64_t private_amount;
    int64_t private_amount_rest;
    int64_t id_deal;
    int64_t xstatus;
    int64_t xstatus2;
    int64_t moment;
    uint64_t moment_ns;
    char client_code[MOEX_ACCOUNT_CAPACITY];
    char login_from[MOEX_ACCOUNT_CAPACITY];
    char comment[MOEX_TEXT_CAPACITY];
    char price_text[MOEX_PRICE_TEXT_CAPACITY];
} MoexPlaza2OwnOrderItem;

typedef struct MoexPlaza2OwnTradeItem {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved1;
    uint8_t multileg;
    uint8_t reserved0[7];
    int64_t id_deal;
    int32_t sess_id;
    int32_t isin_id;
    int64_t amount;
    int64_t public_order_id_buy;
    int64_t public_order_id_sell;
    int64_t private_order_id_buy;
    int64_t private_order_id_sell;
    int64_t moment;
    uint64_t moment_ns;
    char price_text[MOEX_PRICE_TEXT_CAPACITY];
    char rate_price[MOEX_PRICE_TEXT_CAPACITY];
    char swap_price[MOEX_PRICE_TEXT_CAPACITY];
    char code_buy[MOEX_ACCOUNT_CAPACITY];
    char code_sell[MOEX_ACCOUNT_CAPACITY];
    char comment_buy[MOEX_TEXT_CAPACITY];
    char comment_sell[MOEX_TEXT_CAPACITY];
    char login_buy[MOEX_ACCOUNT_CAPACITY];
    char login_sell[MOEX_ACCOUNT_CAPACITY];
} MoexPlaza2OwnTradeItem;

typedef struct MoexPlaza2TwimeReconcilerHealth {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved0;
    uint64_t logical_step;
    uint64_t total_provisional_orders;
    uint64_t total_confirmed_orders;
    uint64_t total_rejected_orders;
    uint64_t total_canceled_orders;
    uint64_t total_filled_orders;
    uint64_t total_diverged_orders;
    uint64_t total_ambiguous_orders;
    uint64_t total_stale_provisional_orders;
    uint64_t total_unmatched_twime_orders;
    uint64_t total_unmatched_plaza_orders;
    uint64_t total_matched_trades;
    uint64_t total_diverged_trades;
    uint64_t total_ambiguous_trades;
    uint64_t total_unmatched_twime_trades;
    uint64_t total_unmatched_plaza_trades;
    uint64_t plaza_revalidation_pending_orders;
    uint64_t plaza_revalidation_pending_trades;
    uint32_t twime_session_state;
    uint8_t twime_present;
    uint8_t twime_transport_open;
    uint8_t twime_session_active;
    uint8_t twime_reject_seen;
    int64_t twime_last_reject_code;
    uint64_t twime_next_expected_inbound_seq;
    uint64_t twime_next_outbound_seq;
    uint64_t twime_reconnect_attempts;
    uint64_t twime_faults;
    uint64_t twime_remote_closes;
    uint64_t twime_last_transition_time_ms;
    uint8_t plaza_present;
    uint8_t plaza_connector_open;
    uint8_t plaza_connector_online;
    uint8_t plaza_snapshot_active;
    uint8_t plaza_required_private_streams_ready;
    uint8_t plaza_invalidated;
    uint8_t reserved1[2];
    uint64_t plaza_last_lifenum;
    uint64_t plaza_required_stream_count;
    uint64_t plaza_online_stream_count;
    uint64_t plaza_snapshot_complete_stream_count;
    char plaza_last_replstate[MOEX_REPLSTATE_CAPACITY];
    char plaza_last_invalidation_reason[MOEX_REASON_CAPACITY];
} MoexPlaza2TwimeReconcilerHealth;

typedef struct MoexPlaza2ReconciledOrderItem {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved2;
    uint8_t status;
    uint8_t match_mode;
    uint8_t last_update_source;
    uint8_t plaza_revalidation_required;
    uint8_t twime_present;
    uint8_t twime_last_kind;
    uint8_t twime_side;
    uint8_t twime_multileg;
    uint8_t twime_has_price;
    uint8_t twime_has_order_qty;
    uint8_t twime_terminal_reject;
    uint8_t twime_terminal_cancel;
    uint8_t plaza_present;
    uint8_t plaza_multileg;
    uint8_t plaza_side;
    uint8_t reserved0;
    uint64_t last_update_logical_sequence;
    uint64_t last_update_logical_step;
    uint64_t twime_cl_ord_id;
    int64_t twime_order_id;
    int64_t twime_prev_order_id;
    int32_t twime_trading_session_id;
    int32_t twime_security_id;
    int32_t twime_cl_ord_link_id;
    int64_t twime_price_mantissa;
    uint32_t twime_order_qty;
    int64_t twime_reject_code;
    uint64_t twime_last_logical_sequence;
    uint64_t twime_last_logical_step;
    int64_t plaza_public_order_id;
    int64_t plaza_private_order_id;
    int32_t plaza_sess_id;
    int32_t plaza_isin_id;
    int64_t plaza_price_mantissa;
    int64_t plaza_public_amount;
    int64_t plaza_public_amount_rest;
    int64_t plaza_private_amount;
    int64_t plaza_private_amount_rest;
    int64_t plaza_id_deal;
    int64_t plaza_xstatus;
    int64_t plaza_xstatus2;
    int32_t plaza_ext_id;
    int8_t plaza_public_action;
    int8_t plaza_private_action;
    uint8_t plaza_from_trade_repl;
    uint8_t plaza_from_user_book;
    uint8_t plaza_from_current_day;
    uint8_t reserved1[3];
    int64_t plaza_moment;
    uint64_t plaza_moment_ns;
    uint64_t plaza_last_logical_sequence;
    char fault_reason[MOEX_REASON_CAPACITY];
    char twime_account[MOEX_ACCOUNT_CAPACITY];
    char twime_compliance_id[MOEX_ACCOUNT_CAPACITY];
    char plaza_client_code[MOEX_ACCOUNT_CAPACITY];
    char plaza_login_from[MOEX_ACCOUNT_CAPACITY];
    char plaza_comment[MOEX_TEXT_CAPACITY];
    char plaza_price_text[MOEX_PRICE_TEXT_CAPACITY];
} MoexPlaza2ReconciledOrderItem;

typedef struct MoexPlaza2ReconciledTradeItem {
    uint32_t struct_size;
    uint16_t abi_version;
    uint16_t reserved2;
    uint8_t status;
    uint8_t match_mode;
    uint8_t last_update_source;
    uint8_t plaza_revalidation_required;
    uint8_t twime_present;
    uint8_t twime_last_kind;
    uint8_t twime_side;
    uint8_t twime_multileg;
    uint8_t twime_has_price;
    uint8_t twime_has_last_qty;
    uint8_t twime_has_order_qty;
    uint8_t plaza_present;
    uint8_t plaza_multileg;
    uint8_t reserved0[5];
    uint64_t last_update_logical_sequence;
    uint64_t last_update_logical_step;
    uint64_t twime_cl_ord_id;
    int64_t twime_order_id;
    int64_t twime_trade_id;
    int32_t twime_trading_session_id;
    int32_t twime_security_id;
    int64_t twime_price_mantissa;
    uint32_t twime_last_qty;
    uint32_t twime_order_qty;
    uint64_t twime_last_logical_sequence;
    uint64_t twime_last_logical_step;
    int64_t plaza_trade_id;
    int32_t plaza_sess_id;
    int32_t plaza_isin_id;
    int64_t plaza_price_mantissa;
    int64_t plaza_amount;
    int64_t plaza_public_order_id_buy;
    int64_t plaza_public_order_id_sell;
    int64_t plaza_private_order_id_buy;
    int64_t plaza_private_order_id_sell;
    int64_t plaza_moment;
    uint64_t plaza_moment_ns;
    uint64_t plaza_last_logical_sequence;
    char fault_reason[MOEX_REASON_CAPACITY];
    char plaza_price_text[MOEX_PRICE_TEXT_CAPACITY];
    char plaza_code_buy[MOEX_ACCOUNT_CAPACITY];
    char plaza_code_sell[MOEX_ACCOUNT_CAPACITY];
    char plaza_comment_buy[MOEX_TEXT_CAPACITY];
    char plaza_comment_sell[MOEX_TEXT_CAPACITY];
    char plaza_login_buy[MOEX_ACCOUNT_CAPACITY];
    char plaza_login_sell[MOEX_ACCOUNT_CAPACITY];
} MoexPlaza2ReconciledTradeItem;

typedef void (*MoexLowRateCallback)(const MoexEventHeader* header, const void* payload, void* user_data);

const char* moex_phase0_abi_name(void);
uint32_t moex_phase0_abi_version(void);
uint8_t moex_environment_start_allowed(const char* environment, uint8_t armed);
uint8_t moex_prod_requires_explicit_arm(void);
MOEX_ABI_DEPRECATED bool moex_phase0_prod_requires_arm(const char* environment, bool armed);
uint32_t moex_sizeof_event_header(void);
uint32_t moex_sizeof_backpressure_counters(void);
uint32_t moex_sizeof_health_snapshot(void);
uint32_t moex_sizeof_connector_create_params(void);
uint32_t moex_sizeof_profile_load_params(void);
uint32_t moex_sizeof_order_submit_request(void);
uint32_t moex_sizeof_order_cancel_request(void);
uint32_t moex_sizeof_order_replace_request(void);
uint32_t moex_sizeof_mass_cancel_request(void);
uint32_t moex_sizeof_subscription_request(void);
uint32_t moex_sizeof_polled_event(void);
uint32_t moex_sizeof_plaza2_private_connector_health(void);
uint32_t moex_sizeof_plaza2_resume_markers(void);
uint32_t moex_sizeof_plaza2_stream_health_item(void);
uint32_t moex_sizeof_plaza2_trading_session_item(void);
uint32_t moex_sizeof_plaza2_instrument_item(void);
uint32_t moex_sizeof_plaza2_matching_map_item(void);
uint32_t moex_sizeof_plaza2_limit_item(void);
uint32_t moex_sizeof_plaza2_position_item(void);
uint32_t moex_sizeof_plaza2_own_order_item(void);
uint32_t moex_sizeof_plaza2_own_trade_item(void);
uint32_t moex_sizeof_plaza2_twime_reconciler_health(void);
uint32_t moex_sizeof_plaza2_reconciled_order_item(void);
uint32_t moex_sizeof_plaza2_reconciled_trade_item(void);
uint32_t moex_alignof_event_header(void);
uint32_t moex_alignof_backpressure_counters(void);
uint32_t moex_alignof_health_snapshot(void);
uint32_t moex_alignof_connector_create_params(void);
uint32_t moex_alignof_profile_load_params(void);
uint32_t moex_alignof_order_submit_request(void);
uint32_t moex_alignof_order_cancel_request(void);
uint32_t moex_alignof_order_replace_request(void);
uint32_t moex_alignof_mass_cancel_request(void);
uint32_t moex_alignof_subscription_request(void);
uint32_t moex_alignof_polled_event(void);
uint32_t moex_alignof_plaza2_private_connector_health(void);
uint32_t moex_alignof_plaza2_resume_markers(void);
uint32_t moex_alignof_plaza2_stream_health_item(void);
uint32_t moex_alignof_plaza2_trading_session_item(void);
uint32_t moex_alignof_plaza2_instrument_item(void);
uint32_t moex_alignof_plaza2_matching_map_item(void);
uint32_t moex_alignof_plaza2_limit_item(void);
uint32_t moex_alignof_plaza2_position_item(void);
uint32_t moex_alignof_plaza2_own_order_item(void);
uint32_t moex_alignof_plaza2_own_trade_item(void);
uint32_t moex_alignof_plaza2_twime_reconciler_health(void);
uint32_t moex_alignof_plaza2_reconciled_order_item(void);
uint32_t moex_alignof_plaza2_reconciled_trade_item(void);
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
MoexResult moex_poll_events_v2(MoexConnectorHandle handle, void* out_events, uint32_t event_stride_bytes,
                               uint32_t capacity, uint32_t* written);
MoexResult moex_poll_events(MoexConnectorHandle handle, void* out_events, uint32_t capacity, uint32_t* written);
MoexResult moex_register_low_rate_callback(MoexConnectorHandle handle, MoexLowRateCallback callback, void* user_data);
MoexResult moex_get_health(MoexConnectorHandle handle, MoexHealthSnapshot* out_health);
MoexResult moex_get_backpressure_counters(MoexConnectorHandle handle, MoexBackpressureCounters* out_counters);
MoexResult moex_flush_recovery_state(MoexConnectorHandle handle);
MoexResult moex_get_plaza2_private_connector_health(MoexConnectorHandle handle,
                                                    MoexPlaza2PrivateConnectorHealth* out_health);
MoexResult moex_get_plaza2_resume_markers(MoexConnectorHandle handle, MoexPlaza2ResumeMarkers* out_markers);
MoexResult moex_get_plaza2_stream_health_count(MoexConnectorHandle handle, uint32_t* out_count);
MoexResult moex_copy_plaza2_stream_health_items(MoexConnectorHandle handle, MoexPlaza2StreamHealthItem* buffer,
                                                uint32_t capacity, uint32_t* written);
MoexResult moex_get_plaza2_trading_session_count(MoexConnectorHandle handle, uint32_t* out_count);
MoexResult moex_copy_plaza2_trading_session_items(MoexConnectorHandle handle, MoexPlaza2TradingSessionItem* buffer,
                                                  uint32_t capacity, uint32_t* written);
MoexResult moex_get_plaza2_instrument_count(MoexConnectorHandle handle, uint32_t* out_count);
MoexResult moex_copy_plaza2_instrument_items(MoexConnectorHandle handle, MoexPlaza2InstrumentItem* buffer,
                                             uint32_t capacity, uint32_t* written);
MoexResult moex_get_plaza2_matching_map_count(MoexConnectorHandle handle, uint32_t* out_count);
MoexResult moex_copy_plaza2_matching_map_items(MoexConnectorHandle handle, MoexPlaza2MatchingMapItem* buffer,
                                               uint32_t capacity, uint32_t* written);
MoexResult moex_get_plaza2_limit_count(MoexConnectorHandle handle, uint32_t* out_count);
MoexResult moex_copy_plaza2_limit_items(MoexConnectorHandle handle, MoexPlaza2LimitItem* buffer, uint32_t capacity,
                                        uint32_t* written);
MoexResult moex_get_plaza2_position_count(MoexConnectorHandle handle, uint32_t* out_count);
MoexResult moex_copy_plaza2_position_items(MoexConnectorHandle handle, MoexPlaza2PositionItem* buffer,
                                           uint32_t capacity, uint32_t* written);
MoexResult moex_get_plaza2_own_order_count(MoexConnectorHandle handle, uint32_t* out_count);
MoexResult moex_copy_plaza2_own_order_items(MoexConnectorHandle handle, MoexPlaza2OwnOrderItem* buffer,
                                            uint32_t capacity, uint32_t* written);
MoexResult moex_get_plaza2_own_trade_count(MoexConnectorHandle handle, uint32_t* out_count);
MoexResult moex_copy_plaza2_own_trade_items(MoexConnectorHandle handle, MoexPlaza2OwnTradeItem* buffer,
                                            uint32_t capacity, uint32_t* written);
MoexResult moex_get_plaza2_twime_reconciler_health(MoexConnectorHandle handle,
                                                   MoexPlaza2TwimeReconcilerHealth* out_health);
MoexResult moex_get_plaza2_reconciled_order_count(MoexConnectorHandle handle, uint32_t* out_count);
MoexResult moex_copy_plaza2_reconciled_order_items(MoexConnectorHandle handle, MoexPlaza2ReconciledOrderItem* buffer,
                                                   uint32_t capacity, uint32_t* written);
MoexResult moex_get_plaza2_reconciled_trade_count(MoexConnectorHandle handle, uint32_t* out_count);
MoexResult moex_copy_plaza2_reconciled_trade_items(MoexConnectorHandle handle, MoexPlaza2ReconciledTradeItem* buffer,
                                                   uint32_t capacity, uint32_t* written);

#ifdef __cplusplus
}
#endif
